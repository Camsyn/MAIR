#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace mair {

namespace {

const Value *getPointerOperand(const Instruction &I) {
  if (const auto *LI = dyn_cast<LoadInst>(&I))
    return LI->getPointerOperand();
  if (const auto *SI = dyn_cast<StoreInst>(&I))
    return SI->getPointerOperand();
  return nullptr;
}

std::optional<uint64_t> getConstantAccessSize(const MemoryAccess &Access) {
  const AbstractAccess *Region = Access.getContiguousRegion();
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

class SCEVExprConverter {
  MAIRGraph &Graph;
  const MAIRLoopInfo &Loops;
  const ObservationSite &Site;

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
                    const ObservationSite &S)
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

void eraseAccess(MemoryAccess &Access) {
  Observation &Parent = *Access.getParent();
  auto It = llvm::find_if(Parent.accesses(), [&](MemoryAccess &Candidate) {
    return &Candidate == &Access;
  });
  assert(It != Parent.end() && "memory access must belong to its parent");
  Parent.erase(It);
}

void addOrMergeAccess(Observation &Target, const MemoryAccess &Access,
                      const MemoryRegion &Region) {
  MemoryAccess NewAccess(Access.getInstructions(), Region, Access.getKind());
  for (MemoryAccess &Existing : Target) {
    if (&Existing.getMemoryObject() != &Access.getMemoryObject())
      continue;
    if (Existing.getKind() != Access.getKind())
      continue;
    if (Existing.getRegion() != Region)
      continue;
    bool Merged = Existing.mergeWith(NewAccess);
    assert(Merged && "identical aggregate accesses must be mergeable");
    (void)Merged;
    return;
  }
  Target.addAccess(NewAccess);
}

std::optional<AbstractAccess> buildContiguousAggregate(
    const MemoryAccess &Access, const SCEVAddRecExpr &Recurrence,
    const SCEV *ExecutionCount, uint64_t AccessSize, ScalarEvolution &SE) {
  const auto *Step = dyn_cast<SCEVConstant>(Recurrence.getStepRecurrence(SE));
  const auto *Count = dyn_cast<SCEVConstant>(ExecutionCount);
  const AbstractAccess *Location = Access.getContiguousRegion();
  if (!Step || !Count || !Location)
    return std::nullopt;

  const APInt &StepValue = Step->getAPInt();
  APInt AbsoluteStep = StepValue.abs();
  if (StepValue.isZero() || AbsoluteStep.getActiveBits() > 64 ||
      AbsoluteStep.getZExtValue() != AccessSize)
    return std::nullopt;

  const APInt &CountValue = Count->getAPInt();
  if (CountValue.isNegative() || CountValue.getActiveBits() > 64)
    return std::nullopt;
  uint64_t Trips = CountValue.getZExtValue();
  if (Trips == 0 || UINT64_MAX / AccessSize < Trips)
    return std::nullopt;

  std::optional<uint64_t> StartOffset =
      matchStartOffset(Recurrence.getStart(), Location->getBase());
  if (!StartOffset)
    return std::nullopt;

  uint64_t RangeOffset = *StartOffset;
  if (StepValue.isNegative()) {
    uint64_t PreviousTrips = Trips - 1;
    if (UINT64_MAX / AccessSize < PreviousTrips)
      return std::nullopt;
    uint64_t Distance = PreviousTrips * AccessSize;
    if (RangeOffset < Distance)
      return std::nullopt;
    RangeOffset -= Distance;
  }

  return AbstractAccess(Location->getBase(),
                        AbstractValue::constant(RangeOffset),
                        AbstractValue::constant(Trips * AccessSize));
}

struct AggregateTarget {
  ObservationSite Site;
  Observation *Event = nullptr;
};

std::optional<AggregateTarget>
getAggregateTarget(MAIRGraph &G, const MAIRLoopInfo &Loops, const Loop &L) {
  std::optional<MAIRLoopExit> Exit = Loops.describeUniqueExit(L);
  if (!Exit)
    return std::nullopt;
  if (Exit->IsDedicated) {
    Observation *Event = G.getBBEntryRepresentative(*Exit->Exit);
    if (!Event || !Event->hasAlwaysGuard())
      return std::nullopt;
    return AggregateTarget{Event->getSite(), Event};
  }

  ObservationSite Site = ObservationSite::edge(*Exit->Exiting, *Exit->Exit);
  for (Observation &Obs : G)
    if (Obs.getSite() == Site && Obs.hasAlwaysGuard())
      return AggregateTarget{Site, &Obs};
  return AggregateTarget{Site, nullptr};
}

bool aggregateOneObservation(MAIRGraph &G, MAIRLoopInfo &Loops,
                             Observation &Obs, MemoryAccess &Access) {
  const AbstractAccess *Location = Access.getContiguousRegion();
  std::optional<uint64_t> AccessSize = getConstantAccessSize(Access);
  if (!Location || !Location->getBase() || !AccessSize || *AccessSize == 0 ||
      Access.getInstructions().empty())
    return false;

  // First bind the access to the loop described by its pointer recurrence,
  // then prove that this MAIR event executes exactly once in that loop.
  ScalarEvolution &SE = Loops.getSE();
  const SCEVAddRecExpr *Recurrence = nullptr;
  for (Instruction *I : Access.getInstructions()) {
    const Value *Pointer = I ? getPointerOperand(*I) : nullptr;
    if (!Pointer)
      return false;
    const auto *Candidate =
        dyn_cast<SCEVAddRecExpr>(SE.getSCEV(const_cast<Value *>(Pointer)));
    if (!Candidate || !Candidate->isAffine() ||
        (Recurrence && Recurrence != Candidate))
      return false;
    Recurrence = Candidate;
  }
  if (!Recurrence || !Recurrence->getLoop())
    return false;

  const Loop &RelevantLoop = *Recurrence->getLoop();
  if (!Loops.isSimpleLoop(RelevantLoop) ||
      !Loops.executesOncePerRelevantIteration(Obs, RelevantLoop))
    return false;
  const SCEV *ExecutionCount =
      Loops.getExactExecutionCountSCEV(Obs, RelevantLoop);
  if (!ExecutionCount)
    return false;
  std::optional<AggregateTarget> Target =
      getAggregateTarget(G, Loops, RelevantLoop);
  if (!Target || !Loops.isValueAvailableAt(*Location->getBase(), Target->Site))
    return false;

  MemoryRegion Aggregate;
  // A constant recurrence that fits the contiguous MAIR abstraction keeps the
  // compact range form. Everything else must survive as an explicit periodic
  // region with no later dependence on LLVM SCEV objects.
  if (std::optional<AbstractAccess> Range = buildContiguousAggregate(
          Access, *Recurrence, ExecutionCount, *AccessSize, SE)) {
    Aggregate = *Range;
  } else {
    const SCEV *Start = Recurrence->getStart();
    const SCEV *Stride = Recurrence->getStepRecurrence(SE);
    if (!Start->getType()->isPointerTy() || !Stride->getType()->isIntegerTy() ||
        !ExecutionCount->getType()->isIntegerTy())
      return false;
    if (const auto *ConstantStride = dyn_cast<SCEVConstant>(Stride))
      if (ConstantStride->getAPInt().isZero())
        return false;

    SCEVExprConverter Converter(G, Loops, Target->Site);
    if (!Converter.canConvert(Start) || !Converter.canConvert(Stride) ||
        !Converter.canConvert(ExecutionCount))
      return false;

    MAIRScalarExpr StartExpr = Converter.convert(Start);
    MAIRScalarExpr StrideExpr = Converter.convert(Stride);
    Type *SizeType = Type::getInt64Ty(Start->getType()->getContext());
    MAIRScalarExpr SizeExpr =
        G.createConstantExpr(*SizeType, APInt(64, *AccessSize));
    MAIRScalarExpr CountExpr = Converter.convert(ExecutionCount);
    Aggregate = PeriodicRegion(Location->getBase(), StartExpr, StrideExpr,
                               SizeExpr, CountExpr);
  }

  Observation *TargetEvent = Target->Event;
  if (!TargetEvent)
    TargetEvent = &G.createObservation(Target->Site);
  addOrMergeAccess(*TargetEvent, Access, Aggregate);
  eraseAccess(Access);
  return true;
}

bool aggregateOneObservation(MAIRGraph &G, MAIRLoopInfo &Loops) {
  if (!Loops.isAvailable())
    return false;
  for (Observation &Obs : G)
    for (MemoryAccess &Access : Obs)
      if (aggregateOneObservation(G, Loops, Obs, Access))
        return true;
  return false;
}

} // namespace

PreservedAnalyses
LoopPeriodicObservationCombinePass::run(MAIRGraph &G,
                                        MAIRGraphAnalysisManager &AM) {
  bool Changed = false;
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  while (aggregateOneObservation(G, Loops))
    Changed = true;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
