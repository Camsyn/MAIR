#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRStatLog.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CheckedArithmetic.h"

using namespace llvm;

#define DEBUG_TYPE "mair-loop-periodic-combine"

STATISTIC(NumAggregatedObservations,
          "Number of observations aggregated across loop iterations");
STATISTIC(NumInputObservations,
          "Number of observations entering loop periodic combination");
STATISTIC(NumRangeAggregates,
          "Number of loop observations aggregated into contiguous ranges");
STATISTIC(NumPeriodicAggregates,
          "Number of loop observations aggregated into periodic regions");

namespace mair {

namespace {

std::optional<uint64_t> matchStartOffset(const SCEV *Start,
                                         const Value *ExpectedBase) {
  if (const auto *Unknown = dyn_cast<SCEVUnknown>(Start))
    if (Unknown->getValue() == ExpectedBase)
      return 0;

  const auto *Add = dyn_cast<SCEVAddExpr>(Start);
  if (!Add)
    return std::nullopt;

  const Value *Base = nullptr;
  std::optional<uint64_t> Offset;
  for (const SCEV *Op : Add->operands()) {
    if (const auto *Unknown = dyn_cast<SCEVUnknown>(Op)) {
      if (Base || Unknown->getValue() != ExpectedBase)
        return std::nullopt;
      Base = Unknown->getValue();
      continue;
    }

    const auto *Const = dyn_cast<SCEVConstant>(Op);
    if (!Const || Offset || Const->getAPInt().isNegative() ||
        Const->getAPInt().getActiveBits() > 64)
      return std::nullopt;
    Offset = Const->getAPInt().getZExtValue();
  }

  if (Base == ExpectedBase)
    return Offset.value_or(0);
  return std::nullopt;
}

/// Whether \p V computes \p Expr and an observation at \p Site may use it.
bool canReuseValue(const SCEV *Expr, const Value *V, ScalarEvolution &SE,
                   const MAIRLoopInfo &Loops, const ProgramPointSite &Site) {
  if (!V || !SE.isSCEVable(V->getType()) || V->getType() != Expr->getType() ||
      !Loops.isValueAvailableAt(*V, Site) ||
      SE.getSCEV(const_cast<Value *>(V)) != Expr)
    return false;
  if (const auto *Insn = dyn_cast<Instruction>(V)) {
    // Poison-generating flags on the candidate do not matter: the
    // observation checks the address the program computes, poison or not
    // (docs/mair-loops.md). canReuseInstruction still rejects candidates
    // whose computed value may differ from Expr, such as a disjoint or.
    SmallVector<Instruction *, 4> DropPoisonGeneratingInsts;
    return SE.canReuseInstruction(Expr, const_cast<Instruction *>(Insn),
                                  DropPoisonGeneratingInsts);
  }
  return true;
}

const Value *findValueForSCEV(const SCEV *Expr, const Observation &Obs,
                              ScalarEvolution &SE, const MAIRLoopInfo &Loops,
                              const ProgramPointSite &Site) {
  if (!Expr)
    return nullptr;
  auto Matches = [&](const Value *V) {
    return canReuseValue(Expr, V, SE, Loops, Site);
  };
  for (Instruction *I : Obs.getInstructions()) {
    const Value *Pointer = I ? llvm::getLoadStorePointerOperand(I) : nullptr;
    if (const auto *GEP = dyn_cast_or_null<GEPOperator>(Pointer))
      if (Matches(GEP->getPointerOperand()))
        return GEP->getPointerOperand();
    if (Matches(Pointer))
      return Pointer;
  }
  const Function *F = Site.getAnchor().getFunction();
  if (!F)
    return nullptr;
  for (const Argument &Arg : F->args())
    if (Matches(&Arg))
      return &Arg;
  for (const BasicBlock &BB : *F)
    for (const Instruction &Insn : BB)
      if (Matches(&Insn))
        return &Insn;
  return nullptr;
}

// The inner AddRec start is often an outer induction, not base+constant.
// Keep that induction as the byte offset of the widened range so the parent
// loop can fold the next AddRec.
std::optional<AbstractValue>
representStartOffset(const SCEV *Start, const Value *ExpectedBase,
                     const Observation &Obs, ScalarEvolution &SE,
                     const MAIRLoopInfo &Loops, const ProgramPointSite &Site) {
  if (std::optional<uint64_t> Offset = matchStartOffset(Start, ExpectedBase))
    return AbstractValue::constant(*Offset);
  const Value *StartPtr = findValueForSCEV(Start, Obs, SE, Loops, Site);
  if (!StartPtr || !ExpectedBase)
    return std::nullopt;

  if (const Value *Index = getByteOffsetIndex(*StartPtr, *ExpectedBase,
                                              Site.getAnchor().getDataLayout()))
    if (Index->getType()->isIntegerTy() &&
        Loops.isValueAvailableAt(*Index, Site))
      return AbstractValue::symbol(*Index);
  if (StartPtr->getType()->isPointerTy() &&
      getUnderlyingObject(StartPtr) == ExpectedBase)
    return AbstractValue::symbol(*StartPtr);
  return std::nullopt;
}

class SCEVExprConverter {
  MAIRGraph &Graph;
  const MAIRLoopInfo &Loops;
  const ProgramPointSite &Site;
  // The loop whose exit Site is, if its trip count counts the backedges with
  // a counter of its own (MAIRLoopInfo::getExitValueExecutionCountSCEV). A
  // recurrence {Start,+,Step} of it has the value of the last iteration
  // there, Start + Step * backedges, backedges being its BackedgeCount.
  // Without such a count recurrences are not converted: the counter is
  // inserted only for loops no induction variable counts.
  const Loop *Counted;

  bool canConvertExitValue(const SCEVAddRecExpr &AddRec) const {
    if (!Counted || AddRec.getLoop() != Counted || !AddRec.isAffine() ||
        !AddRec.getType()->isIntegerTy())
      return false;
    return canConvert(AddRec.getStart()) && canConvert(AddRec.getOperand(1));
  }

  MAIRScalarExpr convertExitValue(const SCEVAddRecExpr &AddRec) {
    auto &Ty = *cast<llvm::IntegerType>(AddRec.getType());
    auto &CountTy = *Type::getInt64Ty(Ty.getContext());
    MAIRScalarExpr Count =
        Graph.createBackedgeCountExpr(*Counted->getHeader(), CountTy);
    // The count is below 2^64, so extending it loses nothing.
    if (Ty.getBitWidth() > CountTy.getBitWidth())
      Count =
          Graph.createUnaryExpr(MAIRScalarExpr::Kind::ZeroExtend, Ty, Count);
    else if (Ty.getBitWidth() < CountTy.getBitWidth())
      Count = Graph.createUnaryExpr(MAIRScalarExpr::Kind::Truncate, Ty, Count);
    const SCEV *Start = AddRec.getStart();
    const SCEV *Step = AddRec.getOperand(1);
    if (!Step->isOne())
      Count = Graph.createBinaryExpr(MAIRScalarExpr::Kind::Mul, Ty,
                                     convert(Step), Count);
    if (Start->isZero())
      return Count;
    return Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add, Ty, convert(Start),
                                  Count);
  }

  bool canConvertNAry(const SCEVNAryExpr &Expr, bool IsAdd) const {
    if (Expr.getNumOperands() < 2)
      return false;
    for (const SCEV *Operand : Expr.operands())
      if (!canConvert(Operand))
        return false;

    Type *ResultType = Expr.getType();
    if (IsAdd && ResultType->isPointerTy()) {
      unsigned PointerOperands = 0;
      Type *IntegerType = nullptr;
      for (const SCEV *Operand : Expr.operands()) {
        Type *OperandType = Operand->getType();
        if (OperandType->isPointerTy()) {
          if (OperandType != ResultType)
            return false;
          ++PointerOperands;
          continue;
        }
        if (!OperandType->isIntegerTy() ||
            (IntegerType && IntegerType != OperandType))
          return false;
        IntegerType = OperandType;
      }
      return PointerOperands == 1 && IntegerType;
    }

    if (!ResultType->isIntegerTy())
      return false;
    return llvm::all_of(Expr.operands(), [&](const SCEV *Operand) {
      return Operand->getType() == ResultType;
    });
  }

  MAIRScalarExpr convertNAry(const SCEVNAryExpr &Expr,
                             MAIRScalarExpr::Kind Kind) {
    auto It = Expr.operands().begin();
    MAIRScalarExpr Result = convert(*It++);
    for (; It != Expr.operands().end(); ++It)
      Result =
          Graph.createBinaryExpr(Kind, *Expr.getType(), Result, convert(*It));
    return Result;
  }

  MAIRScalarExpr convertAdd(const SCEVAddExpr &Add) {
    if (!Add.getType()->isPointerTy())
      return convertNAry(Add, MAIRScalarExpr::Kind::Add);

    const SCEV *PointerOperand = nullptr;
    SmallVector<const SCEV *, 4> IntegerOperands;
    for (const SCEV *Operand : Add.operands()) {
      if (Operand->getType()->isPointerTy())
        PointerOperand = Operand;
      else
        IntegerOperands.push_back(Operand);
    }

    MAIRScalarExpr Offset = convert(IntegerOperands.front());
    Type *OffsetType = IntegerOperands.front()->getType();
    for (const SCEV *Operand : ArrayRef(IntegerOperands).drop_front())
      Offset = Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add, *OffsetType,
                                      Offset, convert(Operand));
    return Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add, *Add.getType(),
                                  convert(PointerOperand), Offset);
  }

public:
  SCEVExprConverter(MAIRGraph &G, const MAIRLoopInfo &L,
                    const ProgramPointSite &S, const Loop *Counted = nullptr)
      : Graph(G), Loops(L), Site(S), Counted(Counted) {}

  bool canConvert(const SCEV *Expr) const {
    if (!Expr || isa<SCEVCouldNotCompute>(Expr))
      return false;
    if (const auto *Constant = dyn_cast<SCEVConstant>(Expr))
      return Constant->getType()->isIntegerTy();
    if (const auto *Unknown = dyn_cast<SCEVUnknown>(Expr)) {
      const Value *V = Unknown->getValue();
      return V &&
             (V->getType()->isIntegerTy() || V->getType()->isPointerTy()) &&
             Loops.isValueAvailableAt(*V, Site);
    }
    if (const auto *Cast = dyn_cast<SCEVIntegralCastExpr>(Expr)) {
      Type *SourceType = Cast->getOperand()->getType();
      Type *ResultType = Cast->getType();
      if (!SourceType->isIntegerTy() || !ResultType->isIntegerTy() ||
          !canConvert(Cast->getOperand()))
        return false;
      unsigned SourceWidth = cast<llvm::IntegerType>(SourceType)->getBitWidth();
      unsigned ResultWidth = cast<llvm::IntegerType>(ResultType)->getBitWidth();
      if (isa<SCEVTruncateExpr>(Cast))
        return ResultWidth < SourceWidth;
      return ResultWidth > SourceWidth;
    }
    if (const auto *Cast = dyn_cast<SCEVPtrToIntExpr>(Expr))
      return Cast->getOperand()->getType()->isPointerTy() &&
             canConvert(Cast->getOperand());
    if (const auto *Add = dyn_cast<SCEVAddExpr>(Expr))
      return canConvertNAry(*Add, true);
    if (const auto *Mul = dyn_cast<SCEVMulExpr>(Expr))
      return canConvertNAry(*Mul, false);
    if (const auto *MinMax = dyn_cast<SCEVMinMaxExpr>(Expr))
      return canConvertNAry(*MinMax, false);
    if (const auto *AddRec = dyn_cast<SCEVAddRecExpr>(Expr))
      return canConvertExitValue(*AddRec);
    if (const auto *UDiv = dyn_cast<SCEVUDivExpr>(Expr)) {
      Type *ResultType = UDiv->getType();
      return ResultType->isIntegerTy() &&
             UDiv->getLHS()->getType() == ResultType &&
             UDiv->getRHS()->getType() == ResultType &&
             canConvert(UDiv->getLHS()) && canConvert(UDiv->getRHS());
    }
    return false;
  }

  MAIRScalarExpr convert(const SCEV *Expr) {
    assert(canConvert(Expr) &&
           "unsupported SCEV must be rejected in preflight");
    if (const auto *Constant = dyn_cast<SCEVConstant>(Expr))
      return Graph.createConstantExpr(*Constant->getType(),
                                      Constant->getAPInt());
    if (const auto *Unknown = dyn_cast<SCEVUnknown>(Expr))
      return Graph.createValueExpr(*Unknown->getValue());
    if (const auto *Cast = dyn_cast<SCEVIntegralCastExpr>(Expr)) {
      MAIRScalarExpr::Kind Kind = MAIRScalarExpr::Kind::SignExtend;
      if (isa<SCEVTruncateExpr>(Cast))
        Kind = MAIRScalarExpr::Kind::Truncate;
      else if (isa<SCEVZeroExtendExpr>(Cast))
        Kind = MAIRScalarExpr::Kind::ZeroExtend;
      return Graph.createUnaryExpr(Kind, *Cast->getType(),
                                   convert(Cast->getOperand()));
    }
    if (const auto *Cast = dyn_cast<SCEVPtrToIntExpr>(Expr))
      return Graph.createUnaryExpr(MAIRScalarExpr::Kind::PtrToInt,
                                   *Cast->getType(),
                                   convert(Cast->getOperand()));
    if (const auto *Add = dyn_cast<SCEVAddExpr>(Expr))
      return convertAdd(*Add);
    if (const auto *Mul = dyn_cast<SCEVMulExpr>(Expr))
      return convertNAry(*Mul, MAIRScalarExpr::Kind::Mul);
    if (const auto *UDiv = dyn_cast<SCEVUDivExpr>(Expr))
      return Graph.createBinaryExpr(MAIRScalarExpr::Kind::UDiv,
                                    *UDiv->getType(), convert(UDiv->getLHS()),
                                    convert(UDiv->getRHS()));
    if (const auto *AddRec = dyn_cast<SCEVAddRecExpr>(Expr))
      return convertExitValue(*AddRec);
    if (const auto *MinMax = dyn_cast<SCEVMinMaxExpr>(Expr)) {
      MAIRScalarExpr::Kind Kind = MAIRScalarExpr::Kind::SMin;
      if (isa<SCEVUMinExpr>(MinMax))
        Kind = MAIRScalarExpr::Kind::UMin;
      else if (isa<SCEVSMaxExpr>(MinMax))
        Kind = MAIRScalarExpr::Kind::SMax;
      else if (isa<SCEVUMaxExpr>(MinMax))
        Kind = MAIRScalarExpr::Kind::UMax;
      return convertNAry(*MinMax, Kind);
    }
    llvm_unreachable("unsupported SCEV passed conversion preflight");
  }
};

/// The product of the unsigned \p Factors in an integer type wide enough that
/// it cannot wrap, or null if there is none. A constant factor counts with
/// its active bits only, so that a count times a small access size stays as
/// narrow as it can.
const SCEV *getUnsignedProduct(ScalarEvolution &SE,
                               ArrayRef<const SCEV *> Factors) {
  unsigned ScratchBits = 0;
  for (const SCEV *Factor : Factors) {
    if (!Factor->getType()->isIntegerTy())
      return nullptr;
    ScratchBits += Factor->getType()->getIntegerBitWidth();
  }
  APInt Max(ScratchBits, 1);
  unsigned Bits = 0;
  for (const SCEV *Factor : Factors) {
    Max *= SE.getUnsignedRangeMax(Factor).zext(ScratchBits);
    if (const auto *Constant = dyn_cast<SCEVConstant>(Factor))
      Bits = std::max(Bits, Constant->getAPInt().getActiveBits());
    else
      Bits = std::max(Bits, Factor->getType()->getIntegerBitWidth());
  }
  Bits = std::max({Bits, Max.getActiveBits(), 1u});
  if (Bits > IntegerType::MAX_INT_BITS)
    return nullptr;
  Type *ProductTy = IntegerType::get(SE.getContext(), Bits);
  SmallVector<const SCEV *, 3> Operands;
  for (const SCEV *Factor : Factors) {
    if (const auto *Constant = dyn_cast<SCEVConstant>(Factor))
      Operands.push_back(
          SE.getConstant(Constant->getAPInt().zextOrTrunc(Bits)));
    else
      Operands.push_back(SE.getNoopOrZeroExtend(Factor, ProductTy));
  }
  return SE.getMulExpr(Operands);
}

/// The SCEV of \p Expr, or null if a leaf has none or the operand types do
/// not fit. The leaves are LLVM values, so an expression the inner loop
/// left in terms of an outer induction comes back as the outer recurrence.
const SCEV *getSCEVOf(const MAIRScalarExpr &Expr, ScalarEvolution &SE) {
  using Kind = MAIRScalarExpr::Kind;
  if (!Expr.isValid())
    return nullptr;
  Type *Ty = Expr.getType();
  switch (Expr.getKind()) {
  case Kind::Constant:
    return SE.getConstant(Expr.getConstant());
  case Kind::LLVMValue: {
    auto *V = const_cast<Value *>(Expr.getLLVMValue());
    return V && SE.isSCEVable(V->getType()) ? SE.getSCEV(V) : nullptr;
  }
  case Kind::Truncate:
  case Kind::ZeroExtend:
  case Kind::SignExtend: {
    const SCEV *Operand = getSCEVOf(Expr.operands().front(), SE);
    if (!Operand || !Operand->getType()->isIntegerTy() || !Ty->isIntegerTy())
      return nullptr;
    unsigned From = Operand->getType()->getIntegerBitWidth();
    unsigned To = Ty->getIntegerBitWidth();
    if (Expr.getKind() == Kind::Truncate)
      return To < From ? SE.getTruncateExpr(Operand, Ty) : nullptr;
    if (To <= From)
      return nullptr;
    return Expr.getKind() == Kind::ZeroExtend
               ? SE.getZeroExtendExpr(Operand, Ty)
               : SE.getSignExtendExpr(Operand, Ty);
  }
  default:
    break;
  }
  if (Expr.operands().size() != 2)
    return nullptr;
  const SCEV *LHS = getSCEVOf(Expr.operands()[0], SE);
  const SCEV *RHS = getSCEVOf(Expr.operands()[1], SE);
  if (!LHS || !RHS)
    return nullptr;
  // A pointer add is a pointer plus an integer offset (MAIRVerifier).
  if (Expr.getKind() == Kind::Add && Ty->isPointerTy())
    return LHS->getType() == Ty && RHS->getType() == SE.getEffectiveSCEVType(Ty)
               ? SE.getAddExpr(LHS, RHS)
               : nullptr;
  if (!Ty->isIntegerTy() || LHS->getType() != Ty || RHS->getType() != Ty)
    return nullptr;
  switch (Expr.getKind()) {
  case Kind::Add:
    return SE.getAddExpr(LHS, RHS);
  case Kind::Mul:
    return SE.getMulExpr(LHS, RHS);
  case Kind::UDiv:
    return SE.getUDivExpr(LHS, RHS);
  case Kind::SMin:
    return SE.getSMinExpr(LHS, RHS);
  case Kind::UMin:
    return SE.getUMinExpr(LHS, RHS);
  case Kind::SMax:
    return SE.getSMaxExpr(LHS, RHS);
  case Kind::UMax:
    return SE.getUMaxExpr(LHS, RHS);
  default:
    return nullptr;
  }
}

/// The start of a periodic region as a MAIR expression. A start that moves
/// with an outer loop is a recurrence, which has none. An IR value that
/// computes it does, or a pointer the access is based on (the pointer
/// operand of its GEP) plus a distance that has one.
std::optional<MAIRScalarExpr>
representPeriodicStart(const SCEV *Start, const Observation &Obs,
                       SCEVExprConverter &Converter, MAIRGraph &ExprGraph,
                       ScalarEvolution &SE, const MAIRLoopInfo &Loops,
                       const ProgramPointSite &Site) {
  if (Converter.canConvert(Start))
    return Converter.convert(Start);
  if (const Value *StartValue = findValueForSCEV(Start, Obs, SE, Loops, Site))
    return ExprGraph.createValueExpr(*StartValue);
  Type *OffsetTy = SE.getEffectiveSCEVType(Start->getType());
  for (Instruction *I : Obs.getInstructions()) {
    const auto *GEP = dyn_cast_or_null<GEPOperator>(
        I ? llvm::getLoadStorePointerOperand(I) : nullptr);
    if (!GEP)
      continue;
    const Value *Candidate = GEP->getPointerOperand();
    if (Candidate->getType() != Start->getType())
      continue;
    const SCEV *CandidateSCEV = SE.getSCEV(const_cast<Value *>(Candidate));
    if (!canReuseValue(CandidateSCEV, Candidate, SE, Loops, Site))
      continue;
    // Pointers into different objects have no distance.
    const SCEV *Distance = SE.getMinusSCEV(Start, CandidateSCEV);
    if (isa<SCEVCouldNotCompute>(Distance) || Distance->getType() != OffsetTy ||
        !Converter.canConvert(Distance))
      continue;
    return ExprGraph.createBinaryExpr(
        MAIRScalarExpr::Kind::Add, *Start->getType(),
        ExprGraph.createValueExpr(*Candidate), Converter.convert(Distance));
  }
  return std::nullopt;
}

void addOrMergeObservation(ProgramPoint &Target, const Observation &Obs,
                           const MemoryRegion &Region,
                           bool MergeEquivalentObservations) {
  Observation NewObservation(Obs.getInstructions(), Region, Obs.getKind(),
                             Obs.getProperties());
  Target.getParent()->getOptimizationConfig().initializeObservation(
      NewObservation);
  if (MergeEquivalentObservations && NewObservation.isMergeable())
    for (Observation &Existing : Target)
      if (&Existing.getMemoryObject() == &Obs.getMemoryObject() &&
          Existing.isMergeable() && Existing.getRegion() == Region &&
          Existing.mergeWith(NewObservation))
        return;
  Target.addObservation(NewObservation);
}

std::optional<AbstractAccess>
buildContiguousAggregate(MAIRGraph &ExprGraph, const Observation &Obs,
                         const SCEVAddRecExpr &Recurrence,
                         const SCEV *ExecutionCount, uint64_t AccessSize,
                         ScalarEvolution &SE, const MAIRLoopInfo &Loops,
                         const ProgramPointSite &Site, const Loop *Counted) {
  const auto *Step = dyn_cast<SCEVConstant>(Recurrence.getStepRecurrence(SE));
  const AbstractAccess *Location = Obs.getContiguousRegion();
  if (!Step || !Location)
    return std::nullopt;

  const APInt &StepValue = Step->getAPInt();
  APInt AbsoluteStep = StepValue.abs();
  if (StepValue.isZero() || AbsoluteStep.getActiveBits() > 64 ||
      AbsoluteStep.getZExtValue() != AccessSize)
    return std::nullopt;

  const auto *Count = dyn_cast<SCEVConstant>(ExecutionCount);
  // Bytes covered by all trips when the trip count is a constant.
  uint64_t RangeSize = 0;
  if (Count) {
    const APInt &CountValue = Count->getAPInt();
    if (CountValue.isNegative() || CountValue.getActiveBits() > 64 ||
        CountValue.isZero())
      return std::nullopt;
    std::optional<uint64_t> Size =
        checkedMulUnsigned(CountValue.getZExtValue(), AccessSize);
    if (!Size)
      return std::nullopt;
    RangeSize = *Size;
  } else if (StepValue.isNegative()) {
    return std::nullopt;
  }

  std::optional<AbstractValue> StartOffset = representStartOffset(
      Recurrence.getStart(), Location->getBase(), Obs, SE, Loops, Site);
  if (!StartOffset)
    return std::nullopt;

  // Every trip starts at an address Location's alignment holds for, and the
  // range starts at the first trip upward and at the last one downward.
  const Align TripAlign = Location->getAlign();
  if (Count) {
    if (StartOffset->isConstant()) {
      uint64_t RangeOffset = StartOffset->getConstant();
      if (StepValue.isNegative()) {
        // The last trip starts one access short of the whole range below
        // the first.
        uint64_t Distance = RangeSize - AccessSize;
        if (RangeOffset < Distance)
          return std::nullopt;
        RangeOffset -= Distance;
      }
      return AbstractAccess(Location->getBase(),
                            AbstractValue::constant(RangeOffset),
                            AbstractValue::constant(RangeSize), TripAlign);
    }
    if (StepValue.isNegative())
      return std::nullopt;
    return AbstractAccess(Location->getBase(), *StartOffset,
                          AbstractValue::constant(RangeSize), TripAlign);
  }
  const SCEV *SizeSCEV = getUnsignedProduct(
      SE, {ExecutionCount,
           SE.getConstant(Type::getInt64Ty(SE.getContext()), AccessSize)});
  SCEVExprConverter Converter(ExprGraph, Loops, Site, Counted);
  if (!SizeSCEV || !Converter.canConvert(SizeSCEV))
    return std::nullopt;
  MAIRScalarExpr SizeExpr = Converter.convert(SizeSCEV);

  AbstractAccess Range(Location->getBase(), *StartOffset, AbstractValue(),
                       TripAlign);
  Range.setSizeExpr(SizeExpr);
  return Range;
}

/// The range of all trips of \p Recurrence when trip i covers \p TripBytes
/// bytes from Recurrence(i) and \p Recurrence steps by exactly that many: the
/// trips abut, so their union is [Recurrence(0), + Count * TripBytes). Each
/// factor of \p TotalFactors is an unsigned count, their product the size.
/// The range starts where the first trip does, which \p TripAlign holds for.
std::optional<AbstractAccess>
buildAbuttingRange(MAIRGraph &ExprGraph, const Observation &Obs,
                   const Value &Base, const SCEVAddRecExpr &Recurrence,
                   const SCEV *TripBytes, ArrayRef<const SCEV *> TotalFactors,
                   Align TripAlign, ScalarEvolution &SE,
                   const MAIRLoopInfo &Loops, const ProgramPointSite &Site,
                   const Loop *Counted) {
  // Addresses wrap modulo the index width, so trip i + 1 starts where trip i
  // ends exactly when the step and the trip size agree in that width.
  const SCEV *Step = Recurrence.getStepRecurrence(SE);
  if (!TripBytes || !TripBytes->getType()->isIntegerTy() ||
      !Step->getType()->isIntegerTy() ||
      SE.getTruncateOrZeroExtend(TripBytes, Step->getType()) != Step)
    return std::nullopt;
  std::optional<AbstractValue> StartOffset =
      representStartOffset(Recurrence.getStart(), &Base, Obs, SE, Loops, Site);
  const SCEV *Total = getUnsignedProduct(SE, TotalFactors);
  if (!StartOffset || !Total || Total->isZero())
    return std::nullopt;
  if (const auto *Constant = dyn_cast<SCEVConstant>(Total))
    if (Constant->getAPInt().getActiveBits() <= 64)
      return AbstractAccess(
          &Base, *StartOffset,
          AbstractValue::constant(Constant->getAPInt().getZExtValue()),
          TripAlign);
  SCEVExprConverter Converter(ExprGraph, Loops, Site, Counted);
  if (!Converter.canConvert(Total))
    return std::nullopt;
  AbstractAccess Range(&Base, *StartOffset, AbstractValue(), TripAlign);
  Range.setSizeExpr(Converter.convert(Total));
  return Range;
}

/// A range an inner loop summarized whose size S is an expression, such as
/// the row [A + 4ni, + 4n) of A[i][k] over k. If the parent loop advances its
/// start by S, the rows abut: [A, + Count * S).
std::optional<AbstractAccess>
buildNestedRangeAggregate(MAIRGraph &ExprGraph, const Observation &Obs,
                          const SCEVAddRecExpr &Recurrence,
                          const SCEV *ExecutionCount, ScalarEvolution &SE,
                          const MAIRLoopInfo &Loops,
                          const ProgramPointSite &Site, const Loop *Counted) {
  const AbstractAccess &Location = *Obs.getContiguousRegion();
  const SCEV *Size = getSCEVOf(Location.getSizeExpr(), SE);
  if (!Size || !SE.isLoopInvariant(Size, Recurrence.getLoop()))
    return std::nullopt;
  return buildAbuttingRange(ExprGraph, Obs, *Location.getBase(), Recurrence,
                            Size, {ExecutionCount, Size}, Location.getAlign(),
                            SE, Loops, Site, Counted);
}

/// A periodic region an inner loop summarized: T trips of access size a,
/// stride S, starting at s(j), such as the column of B[k][j] over k. If the
/// parent loop advances s(j) by a and its Count iterations cover a stride,
/// Count * a == S, then for each trip k the iterations j fill
/// [s(0) + k * S, + S), and the trips abut: [s(0), + T * Count * a).
std::optional<AbstractAccess> buildNestedPeriodicAggregate(
    MAIRGraph &ExprGraph, const Observation &Obs,
    const PeriodicRegion &Periodic, const SCEVAddRecExpr &Recurrence,
    const SCEV *ExecutionCount, ScalarEvolution &SE, const MAIRLoopInfo &Loops,
    const ProgramPointSite &Site, const Loop *Counted) {
  const Loop *L = Recurrence.getLoop();
  const SCEV *Stride = getSCEVOf(Periodic.getStride(), SE);
  const SCEV *Size = getSCEVOf(Periodic.getAccessSize(), SE);
  const SCEV *Trips = getSCEVOf(Periodic.getTripCount(), SE);
  Type *StepTy = Recurrence.getStepRecurrence(SE)->getType();
  if (!Stride || !Size || !Trips || !Stride->getType()->isIntegerTy() ||
      !Size->getType()->isIntegerTy() || !StepTy->isIntegerTy() ||
      !SE.isLoopInvariant(Stride, L) || !SE.isLoopInvariant(Size, L) ||
      !SE.isLoopInvariant(Trips, L))
    return std::nullopt;
  const SCEV *Run =
      SE.getMulExpr(SE.getTruncateOrZeroExtend(ExecutionCount, StepTy),
                    SE.getTruncateOrZeroExtend(Size, StepTy));
  if (Run != SE.getTruncateOrSignExtend(Stride, StepTy))
    return std::nullopt;
  return buildAbuttingRange(ExprGraph, Obs, *Periodic.getBase(), Recurrence,
                            Size, {Trips, ExecutionCount, Size},
                            Periodic.getAlign(), SE, Loops, Site, Counted);
}

struct AggregatePlan {
  ProgramPointSite TargetSite;
  // Null if the parent graph has no unguarded program point at the site yet.
  ProgramPoint *Target = nullptr;
  MemoryRegion Region;
  bool IsRange = false;
  // The trip count was read off induction variables at the exit, or counts
  // the backedges with a counter materialization inserts.
  bool UsesExitTrip = false;
  bool UsesExitCounter = false;
};

std::optional<AggregatePlan> buildAggregatePlan(MAIRGraph &G,
                                                MAIRLoopInfo &Loops,
                                                ProgramPoint &PP,
                                                Observation &Obs) {
  const AbstractAccess *Location = Obs.getContiguousRegion();
  const PeriodicRegion *Periodic = Obs.getPeriodicRegion();
  const Value *Base = Location   ? Location->getBase()
                      : Periodic ? Periodic->getBase()
                                 : nullptr;
  if (!Obs.isMovable() || !Base || Obs.getInstructions().empty())
    return std::nullopt;
  // A region an inner loop summarized: a range whose size is an expression,
  // or a periodic region. Both are folded only into a range (buildNested*).
  const bool IsNested = Periodic || Location->hasSizeExpr();
  if (!IsNested && (!Location->getSize().isConstant() ||
                    Location->getSize().getConstant() == 0))
    return std::nullopt;
  const uint64_t AccessSize = IsNested ? 0 : Location->getSize().getConstant();

  // Peel one affine AddRec of this loop. A range produced by an inner loop
  // keeps the outer induction as its start, so the address SCEV — not the
  // original inner pointer — is the recurrence of the parent loop. So is the
  // start of a periodic region.
  ScalarEvolution &SE = Loops.getSE();
  const SCEVAddRecExpr *Recurrence = nullptr;
  if (const SCEV *Address =
          Periodic ? getSCEVOf(Periodic->getStart(), SE)
                   : Location->getAddressSCEV(
                         SE, AbstractAccess::ZeroOffsetBase::Analyzed)) {
    if (const auto *AddRec = dyn_cast<SCEVAddRecExpr>(Address))
      if (AddRec->isAffine() && AddRec->getLoop() == G.getLoop())
        Recurrence = AddRec;
  }
  if (!Recurrence && !IsNested) {
    for (Instruction *I : Obs.getInstructions()) {
      const Value *Pointer = I ? llvm::getLoadStorePointerOperand(I) : nullptr;
      if (!Pointer)
        return std::nullopt;
      const auto *Candidate =
          dyn_cast<SCEVAddRecExpr>(SE.getSCEV(const_cast<Value *>(Pointer)));
      if (!Candidate || !Candidate->isAffine() ||
          (Recurrence && Recurrence != Candidate))
        return std::nullopt;
      Recurrence = Candidate;
    }
  }
  if (!Recurrence || Recurrence->getLoop() != G.getLoop())
    return std::nullopt;

  const Loop &RelevantLoop = *Recurrence->getLoop();
  std::optional<ProgramPointSite> TargetSite = Loops.getExitSite(RelevantLoop);
  if (!TargetSite || !Loops.canSinkToExit(PP, RelevantLoop) ||
      !Loops.isValueAvailableAt(*Base, *TargetSite))
    return std::nullopt;
  // Without a computable trip count, read the count off induction variables
  // at the exit (docs/mair-loops.md, 数据相关的出口).
  const SCEV *ExecutionCount =
      Loops.getExactExecutionCountSCEV(PP, RelevantLoop);
  const bool UsesExitTrip = !ExecutionCount;
  bool UsesExitCounter = false;
  if (UsesExitTrip)
    ExecutionCount = Loops.getExitValueExecutionCountSCEV(
        PP, RelevantLoop, *TargetSite, &UsesExitCounter);
  if (!ExecutionCount)
    return std::nullopt;
  const Loop *Counted = UsesExitCounter ? &RelevantLoop : nullptr;

  MemoryRegion Aggregate;
  // A constant recurrence that fits the contiguous MAIR abstraction keeps the
  // compact range form. Everything else must survive as an explicit periodic
  // region with no later dependence on LLVM SCEV objects.
  bool IsRangeAggregate = false;
  if (IsNested) {
    std::optional<AbstractAccess> Range =
        Periodic ? buildNestedPeriodicAggregate(*G.getParent(), Obs, *Periodic,
                                                *Recurrence, ExecutionCount, SE,
                                                Loops, *TargetSite, Counted)
                 : buildNestedRangeAggregate(*G.getParent(), Obs, *Recurrence,
                                             ExecutionCount, SE, Loops,
                                             *TargetSite, Counted);
    if (!Range)
      return std::nullopt;
    Aggregate = *Range;
    IsRangeAggregate = true;
  } else if (std::optional<AbstractAccess> Range = buildContiguousAggregate(
                 *G.getParent(), Obs, *Recurrence, ExecutionCount, AccessSize,
                 SE, Loops, *TargetSite, Counted)) {
    Aggregate = *Range;
    IsRangeAggregate = true;
  } else {
    const SCEV *Start = Recurrence->getStart();
    const SCEV *Stride = Recurrence->getStepRecurrence(SE);
    if (!Start->getType()->isPointerTy() || !Stride->getType()->isIntegerTy() ||
        !ExecutionCount->getType()->isIntegerTy())
      return std::nullopt;
    if (const auto *ConstantStride = dyn_cast<SCEVConstant>(Stride))
      if (ConstantStride->getAPInt().isZero())
        return std::nullopt;

    SCEVExprConverter Converter(*G.getParent(), Loops, *TargetSite, Counted);
    if (!Converter.canConvert(Stride) || !Converter.canConvert(ExecutionCount))
      return std::nullopt;
    std::optional<MAIRScalarExpr> StartExpr = representPeriodicStart(
        Start, Obs, Converter, *G.getParent(), SE, Loops, *TargetSite);
    if (!StartExpr)
      return std::nullopt;
    MAIRScalarExpr StrideExpr = Converter.convert(Stride);
    Type *SizeType = Type::getInt64Ty(Start->getType()->getContext());
    MAIRScalarExpr SizeExpr =
        G.getParent()->createConstantExpr(*SizeType, APInt(64, AccessSize));
    MAIRScalarExpr CountExpr = Converter.convert(ExecutionCount);
    // Each trip starts where Location does in that iteration.
    Aggregate = PeriodicRegion(Location->getBase(), *StartExpr, StrideExpr,
                               SizeExpr, CountExpr, Location->getAlign());
  }

  return AggregatePlan{
      *TargetSite,
      Loops.findExitPoint(RelevantLoop, *TargetSite, AlwaysGuard{}),
      std::move(Aggregate),
      IsRangeAggregate,
      UsesExitTrip,
      UsesExitCounter};
}

void commitAggregate(MAIRGraph &G, Observation &Obs, AggregatePlan Plan,
                     bool MergeEquivalentObservations) {
  ProgramPoint *TargetPP = Plan.Target;
  if (!TargetPP)
    TargetPP = &G.getParent()->createProgramPoint(Plan.TargetSite);
  addOrMergeObservation(*TargetPP, Obs, Plan.Region,
                        MergeEquivalentObservations);
  Obs.getParent()->erase(Obs);
  ++NumAggregatedObservations;
  if (Plan.UsesExitTrip)
    MAIRStatScope::add(MAIRStatField::ExitTrip);
  if (Plan.UsesExitCounter)
    MAIRStatScope::add(MAIRStatField::ExitCounter);
  if (Plan.IsRange) {
    ++NumRangeAggregates;
    MAIRStatScope::add(MAIRStatField::Range);
  } else {
    ++NumPeriodicAggregates;
    MAIRStatScope::add(MAIRStatField::Periodic);
  }
}

const SCEVAddRecExpr *findAffineRecurrenceOf(const Observation &Obs,
                                             const Loop &L,
                                             ScalarEvolution &SE) {
  auto AsRecurrence = [&](const SCEV *Expr) -> const SCEVAddRecExpr * {
    const auto *AddRec = dyn_cast_or_null<SCEVAddRecExpr>(Expr);
    return AddRec && AddRec->isAffine() && AddRec->getLoop() == &L ? AddRec
                                                                   : nullptr;
  };
  if (const AbstractAccess *Location = Obs.getContiguousRegion())
    if (const SCEVAddRecExpr *AddRec = AsRecurrence(Location->getAddressSCEV(
            SE, AbstractAccess::ZeroOffsetBase::Analyzed)))
      return AddRec;
  for (Instruction *I : Obs.getInstructions())
    if (const Value *Pointer =
            I ? llvm::getLoadStorePointerOperand(I) : nullptr)
      if (const SCEVAddRecExpr *AddRec =
              AsRecurrence(SE.getSCEV(const_cast<Value *>(Pointer))))
        return AddRec;
  return nullptr;
}

// Why buildAggregatePlan left \p Obs in the loop of \p G, for -mair-stats.
// The checks are diagnostic: in the order below, the first that fails names
// the reason. Loop-invariant observations are left to LIOM and not counted.
std::optional<MAIRStatField> classifyRejection(MAIRGraph &G,
                                               MAIRLoopInfo &Loops,
                                               const ProgramPoint &PP,
                                               const Observation &Obs) {
  const Loop &L = *G.getLoop();
  if (Loops.isLoopInvariant(Obs, L))
    return std::nullopt;
  if (!Obs.isMovable() || !Obs.getContiguousRegion())
    return MAIRStatField::RejectOther;
  if (G.hasForwardBarrier() || G.hasBackwardBarrier())
    return MAIRStatField::RejectBarrier;
  const SCEVAddRecExpr *Recurrence =
      findAffineRecurrenceOf(Obs, L, Loops.getSE());
  if (!Recurrence)
    return MAIRStatField::RejectNonAffine;
  if (!Loops.executesOncePerRelevantIteration(PP, L))
    return MAIRStatField::RejectConditional;
  if (!L.getExitingBlock() || !L.getUniqueExitBlock())
    return MAIRStatField::RejectMultiExit;
  std::optional<ProgramPointSite> Site = Loops.getExitSite(L);
  if (!Loops.getExactExecutionCountSCEV(PP, L) &&
      (!Site || !Loops.getExitValueExecutionCountSCEV(PP, L, *Site)))
    return MAIRStatField::RejectNoCount;
  return MAIRStatField::RejectOther;
}

bool aggregateObservations(MAIRGraph &G, MAIRLoopInfo &Loops,
                           bool MergeEquivalentObservations) {
  if (!Loops.isAvailable() || !G.getLoop())
    return false;

  bool Changed = false;
  for (ProgramPoint &PP : G) {
    for (Observation &Obs : make_early_inc_range(PP)) {
      std::optional<AggregatePlan> Plan = buildAggregatePlan(G, Loops, PP, Obs);
      if (!Plan) {
        if (mairStatsEnabled())
          if (std::optional<MAIRStatField> Reason =
                  classifyRejection(G, Loops, PP, Obs))
            MAIRStatScope::add(*Reason);
        continue;
      }
      commitAggregate(G, Obs, std::move(*Plan), MergeEquivalentObservations);
      Changed = true;
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses
LoopPeriodicObservationCombinePass::run(MAIRGraph &G,
                                        MAIRGraphAnalysisManager &AM) {
  MAIRStatScope Scope("lpoc", Ordinal, G, NumInputObservations, [&] {
    return MAIRStat::compute(G).getNumObservations();
  });
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  bool Changed = aggregateObservations(G, Loops, MergeEquivalentObservations);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
