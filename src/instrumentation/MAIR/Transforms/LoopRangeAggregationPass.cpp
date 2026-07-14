#include "MAIR/Transforms/LoopRangeAggregationPass.h"
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

void eraseAccess(MemoryAccess &Access) {
  Observation &Parent = *Access.getParent();
  auto It = llvm::find_if(Parent.accesses(), [&](MemoryAccess &Candidate) {
    return &Candidate == &Access;
  });
  assert(It != Parent.end() && "memory access must belong to its parent");
  Parent.erase(It);
}

void addOrMergeRangeAccess(Observation &Target, const MemoryAccess &Access,
                           const AbstractAccess &Range) {
  MemoryAccess NewAccess(Access.getInstructions(), Range, Access.getKind());
  for (MemoryAccess &Existing : Target) {
    if (&Existing.getMemoryObject() != &Access.getMemoryObject())
      continue;
    if (Existing.getKind() != Access.getKind())
      continue;
    const AbstractAccess *ExistingRegion = Existing.getContiguousRegion();
    if (!ExistingRegion || *ExistingRegion != Range)
      continue;
    bool Merged = Existing.mergeWith(NewAccess);
    assert(Merged && "identical range accesses must be mergeable");
    (void)Merged;
    return;
  }
  Target.addAccess(NewAccess);
}

bool aggregateOneRange(MAIRLoopInfo &Loops, Observation &Obs,
                       MemoryAccess &Access) {
  if (Access.getInstructions().size() != 1)
    return false;
  const Instruction *I = Access.getInstruction();
  if (!I)
    return false;
  const Value *Ptr = getPointerOperand(*I);
  if (!Ptr)
    return false;

  Loop *L = Loops.getLoopFor(Obs);
  if (!L || !Loops.isSimpleLoop(*L))
    return false;
  if (!Loops.isEveryIterationAccess(Access, *L))
    return false;
  if (!Loops.isGuaranteedOnLoopEntry(Access, *L))
    return false;

  std::optional<uint64_t> AccessSize = getConstantAccessSize(Access);
  if (!AccessSize || *AccessSize == 0)
    return false;

  // Recognize one affine pointer recurrence whose iterations cover a
  // contiguous byte range. General periodic recurrences remain unchanged.
  ScalarEvolution &SE = Loops.getSE();
  const auto *AR =
      dyn_cast<SCEVAddRecExpr>(SE.getSCEV(const_cast<Value *>(Ptr)));
  if (!AR || AR->getLoop() != L || !AR->isAffine())
    return false;

  const auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE));
  if (!Step)
    return false;
  const APInt &StepValue = Step->getAPInt();
  APInt AbsoluteStep = StepValue.abs();
  if (AbsoluteStep.getActiveBits() > 64)
    return false;
  uint64_t StepBytes = AbsoluteStep.getZExtValue();
  if (StepBytes != *AccessSize)
    return false;

  const AbstractAccess *Loc = Access.getContiguousRegion();
  if (!Loc)
    return false;
  std::optional<uint64_t> StartOffset =
      matchStartOffset(AR->getStart(), Loc->getBase());
  if (!StartOffset)
    return false;

  const auto *BackedgeCount =
      dyn_cast<SCEVConstant>(SE.getBackedgeTakenCount(L));
  if (!BackedgeCount)
    return false;
  const APInt &BackedgeValue = BackedgeCount->getAPInt();
  if (BackedgeValue.isNegative() || BackedgeValue.getActiveBits() > 64)
    return false;

  uint64_t BackedgeCountValue = BackedgeValue.getZExtValue();
  if (BackedgeCountValue == UINT64_MAX)
    return false;
  uint64_t TripCount = BackedgeCountValue + 1;
  if (UINT64_MAX / *AccessSize < TripCount)
    return false;
  uint64_t RangeSize = TripCount * *AccessSize;

  // A decreasing recurrence starts at the high end. Normalize it to the
  // lowest byte offset used by the loop before creating the MAIR range.
  uint64_t RangeOffset = *StartOffset;
  if (StepValue.isNegative()) {
    if (UINT64_MAX / *AccessSize < BackedgeCountValue)
      return false;
    uint64_t Distance = BackedgeCountValue * *AccessSize;
    if (RangeOffset < Distance)
      return false;
    RangeOffset -= Distance;
  }

  // A shared exit would execute the aggregate on paths that never entered the
  // loop. Leave that critical-edge case for a virtual exit plus instantiate.
  if (!Loops.hasDedicatedExit(*L))
    return false;
  Observation *Target = Loops.getUniqueExitEntry(*L);
  if (!Target || Target == &Obs)
    return false;

  AbstractAccess Range(Loc->getBase(), AbstractValue::constant(RangeOffset),
                       AbstractValue::constant(RangeSize));
  addOrMergeRangeAccess(*Target, Access, Range);
  eraseAccess(Access);
  return true;
}

bool aggregateOneRange(MAIRGraph &G, MAIRLoopInfo &Loops) {
  if (!Loops.isAvailable())
    return false;
  for (Observation &Obs : G)
    for (MemoryAccess &Access : Obs)
      if (aggregateOneRange(Loops, Obs, Access))
        return true;
  return false;
}

} // namespace

PreservedAnalyses LoopRangeAggregationPass::run(MAIRGraph &G,
                                                MAIRGraphAnalysisManager &AM) {
  bool Changed = false;
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  while (aggregateOneRange(G, Loops))
    Changed = true;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
