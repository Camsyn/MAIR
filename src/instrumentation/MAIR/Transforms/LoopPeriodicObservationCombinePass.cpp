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

std::optional<uint64_t> getConstantAccessSize(const Observation &Obs) {
  const AbstractAccess *Region = Obs.getContiguousRegion();
  if (!Region)
    return std::nullopt;
  const AbstractValue &Size = Region->getSize();
  if (!Size.isConstant())
    return std::nullopt;
  return Size.getConstant();
}

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
    if (!Const)
      return std::nullopt;
    if (Const->getAPInt().isNegative())
      return std::nullopt;
    if (Const->getAPInt().getActiveBits() > 64)
      return std::nullopt;
    if (Offset)
      return std::nullopt;
    Offset = Const->getAPInt().getZExtValue();
  }

  if (Base == ExpectedBase)
    return Offset.value_or(0);
  return std::nullopt;
}

const Value *findValueForSCEV(const SCEV *Expr, const Observation &Obs,
                              ScalarEvolution &SE, const MAIRLoopInfo &Loops,
                              const ProgramPointSite &Site) {
  if (!Expr)
    return nullptr;
  auto Matches = [&](const Value *V) {
    if (!V || !SE.isSCEVable(V->getType()) || V->getType() != Expr->getType() ||
        !Loops.isValueAvailableAt(*V, Site) ||
        SE.getSCEV(const_cast<Value *>(V)) != Expr)
      return false;
    if (const auto *Insn = dyn_cast<Instruction>(V)) {
      SmallVector<Instruction *, 4> DropPoisonGeneratingInsts;
      return SE.canReuseInstruction(Expr, const_cast<Instruction *>(Insn),
                                    DropPoisonGeneratingInsts) &&
             DropPoisonGeneratingInsts.empty();
    }
    return true;
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
                    const ProgramPointSite &S)
      : Graph(G), Loops(L), Site(S) {}

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
    if (const auto *Add = dyn_cast<SCEVAddExpr>(Expr))
      return canConvertNAry(*Add, true);
    if (const auto *Mul = dyn_cast<SCEVMulExpr>(Expr))
      return canConvertNAry(*Mul, false);
    if (const auto *MinMax = dyn_cast<SCEVMinMaxExpr>(Expr))
      return canConvertNAry(*MinMax, false);
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
    if (const auto *Add = dyn_cast<SCEVAddExpr>(Expr))
      return convertAdd(*Add);
    if (const auto *Mul = dyn_cast<SCEVMulExpr>(Expr))
      return convertNAry(*Mul, MAIRScalarExpr::Kind::Mul);
    if (const auto *UDiv = dyn_cast<SCEVUDivExpr>(Expr))
      return Graph.createBinaryExpr(MAIRScalarExpr::Kind::UDiv,
                                    *UDiv->getType(), convert(UDiv->getLHS()),
                                    convert(UDiv->getRHS()));
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

void addOrMergeObservation(ProgramPoint &Target, const Observation &Obs,
                           const MemoryRegion &Region,
                           bool MergeEquivalentObservations) {
  Observation NewObservation(Obs.getInstructions(), Region, Obs.getKind(),
                             Obs.getProperties());
  Target.getParent()->getOptimizationConfig().initializeObservation(
      NewObservation);
  if (MergeEquivalentObservations) {
    for (Observation &Existing : Target) {
      if (&Existing.getMemoryObject() != &Obs.getMemoryObject())
        continue;
      if (!Existing.isMergeable() || !NewObservation.isMergeable())
        continue;
      if (Existing.getRegion() != Region)
        continue;
      bool Merged = Existing.mergeWith(NewObservation);
      if (Merged)
        return;
    }
  }
  Target.addObservation(NewObservation);
}

std::optional<AbstractAccess> buildContiguousAggregate(
    MAIRGraph &ExprGraph, const Observation &Obs,
                         const SCEVAddRecExpr &Recurrence,
                         const SCEV *ExecutionCount, uint64_t AccessSize,
                         ScalarEvolution &SE, const MAIRLoopInfo &Loops,
                         const ProgramPointSite &Site) {
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
                            AbstractValue::constant(RangeSize));
    }
    if (StepValue.isNegative())
      return std::nullopt;
    return AbstractAccess(Location->getBase(), *StartOffset,
                          AbstractValue::constant(RangeSize));
  }
  if (!StartOffset->isConstant())
    return std::nullopt;

  const SCEV *SizeSCEV = ExecutionCount;
  if (AccessSize != 1) {
    unsigned CountBits = ExecutionCount->getType()->getIntegerBitWidth();
    unsigned ScratchBits = CountBits + 64;
    APInt Scale(ScratchBits, AccessSize);
    APInt MaxSize =
        SE.getUnsignedRangeMax(ExecutionCount).zext(ScratchBits) * Scale;
    unsigned SizeBits = std::max(
        CountBits, std::max(Scale.getActiveBits(), MaxSize.getActiveBits()));
    if (SizeBits > IntegerType::MAX_INT_BITS)
      return std::nullopt;
    Type *SizeTy =
        IntegerType::get(ExecutionCount->getType()->getContext(), SizeBits);
    if (SizeBits > CountBits)
      SizeSCEV = SE.getZeroExtendExpr(SizeSCEV, SizeTy);
    SizeSCEV = SE.getMulExpr(SizeSCEV, SE.getConstant(SizeTy, AccessSize));
  }
  SCEVExprConverter Converter(ExprGraph, Loops, Site);
  if (!Converter.canConvert(SizeSCEV))
    return std::nullopt;
  MAIRScalarExpr SizeExpr = Converter.convert(SizeSCEV);

  AbstractAccess Range(Location->getBase(),
                       AbstractValue::constant(StartOffset->getConstant()),
                       AbstractValue());
  Range.setSizeExprNode(SizeExpr.getOpaqueNode());
  return Range;
}

struct AggregatePlan {
  ProgramPointSite TargetSite;
  // Null if the parent graph has no unguarded program point at the site yet.
  ProgramPoint *Target = nullptr;
  MemoryRegion Region;
  bool IsRange = false;
};

std::optional<AggregatePlan> buildAggregatePlan(MAIRGraph &G,
                                                MAIRLoopInfo &Loops,
                                                ProgramPoint &PP,
                                                Observation &Obs) {
  const AbstractAccess *Location = Obs.getContiguousRegion();
  std::optional<uint64_t> AccessSize = getConstantAccessSize(Obs);
  if (!Obs.isMovable() || !Location || !Location->getBase() || !AccessSize ||
      *AccessSize == 0 || Obs.getInstructions().empty())
    return std::nullopt;

  // Peel one affine AddRec of this loop. A range produced by an inner loop
  // keeps the outer induction as its start, so the address SCEV — not the
  // original inner pointer — is the recurrence of the parent loop.
  ScalarEvolution &SE = Loops.getSE();
  const SCEVAddRecExpr *Recurrence = nullptr;
  if (const SCEV *Address = Location->getAddressSCEV(
          SE, AbstractAccess::ZeroOffsetBase::Analyzed)) {
    if (const auto *AddRec = dyn_cast<SCEVAddRecExpr>(Address))
      if (AddRec->isAffine() && AddRec->getLoop() == G.getLoop())
        Recurrence = AddRec;
  }
  if (!Recurrence) {
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
  if (!Recurrence || !Recurrence->getLoop() ||
      Recurrence->getLoop() != G.getLoop())
    return std::nullopt;

  const Loop &RelevantLoop = *Recurrence->getLoop();
  const SCEV *ExecutionCount =
      Loops.getExactExecutionCountSCEV(PP, RelevantLoop);
  if (!ExecutionCount)
    return std::nullopt;
  std::optional<ProgramPointSite> TargetSite = Loops.getExitSite(RelevantLoop);
  if (!TargetSite || !Loops.canSinkToExit(PP, RelevantLoop) ||
      !Loops.isValueAvailableAt(*Location->getBase(), *TargetSite))
    return std::nullopt;

  MemoryRegion Aggregate;
  // A constant recurrence that fits the contiguous MAIR abstraction keeps the
  // compact range form. Everything else must survive as an explicit periodic
  // region with no later dependence on LLVM SCEV objects.
  bool IsRangeAggregate = false;
  if (std::optional<AbstractAccess> Range = buildContiguousAggregate(
          *G.getParent(), Obs, *Recurrence, ExecutionCount, *AccessSize, SE,
          Loops, *TargetSite)) {
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

    SCEVExprConverter Converter(*G.getParent(), Loops, *TargetSite);
    if (!Converter.canConvert(Start) || !Converter.canConvert(Stride) ||
        !Converter.canConvert(ExecutionCount))
      return std::nullopt;

    MAIRScalarExpr StartExpr = Converter.convert(Start);
    MAIRScalarExpr StrideExpr = Converter.convert(Stride);
    Type *SizeType = Type::getInt64Ty(Start->getType()->getContext());
    MAIRScalarExpr SizeExpr =
        G.getParent()->createConstantExpr(*SizeType, APInt(64, *AccessSize));
    MAIRScalarExpr CountExpr = Converter.convert(ExecutionCount);
    Aggregate = PeriodicRegion(Location->getBase(), StartExpr, StrideExpr,
                               SizeExpr, CountExpr);
  }

  return AggregatePlan{
      *TargetSite,
      Loops.findExitPoint(RelevantLoop, *TargetSite, AlwaysGuard{}),
      std::move(Aggregate), IsRangeAggregate};
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
  if (Plan.IsRange) {
    ++NumRangeAggregates;
    MAIRStatScope::add(MAIRStatField::Range);
  } else {
    ++NumPeriodicAggregates;
    MAIRStatScope::add(MAIRStatField::Periodic);
  }
}

bool aggregateObservations(MAIRGraph &G, MAIRLoopInfo &Loops,
                           bool MergeEquivalentObservations) {
  if (!Loops.isAvailable() || !G.getLoop())
    return false;

  bool Changed = false;
  for (ProgramPoint &PP : G) {
    for (auto It = PP.begin(); It != PP.end();) {
      Observation &Obs = *It++;
      std::optional<AggregatePlan> Plan = buildAggregatePlan(G, Loops, PP, Obs);
      if (!Plan)
        continue;
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
  const unsigned NumInputs = MAIRStat::compute(G).getNumObservations();
  NumInputObservations += NumInputs;
  MAIRStatScope Scope("lpoc", Ordinal, G, NumInputs);
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  bool Changed = aggregateObservations(G, Loops, MergeEquivalentObservations);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
