#include "MAIR/Analysis/LoopAnalysis.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

AnalysisKey LoopAnalysis::Key;

MAIRLoopInfo::MAIRLoopInfo(MAIRGraph &G, FunctionAnalysisManager &Manager)
    : Graph(&G), FAM(&Manager), Func(G.getMutableFunction()) {
  if (!Func)
    return;
  LI = &G.getLoopInfo();
  DT = &G.getDominatorTree();
  PDT = &G.getPostDominatorTree();
}

Loop *MAIRLoopInfo::getLoopFor(const ProgramPoint &PP) const {
  if (!isAvailable())
    return nullptr;
  const BasicBlock *BB = PP.getBasicBlock();
  return BB ? LI->getLoopFor(BB) : nullptr;
}

bool MAIRLoopInfo::isValueAvailableAt(const Value &V,
                                      const ProgramPointSite &Site) const {
  return isAvailable() && mair::isValueAvailableAt(V, Site.getAnchor(), *DT);
}

bool MAIRLoopInfo::isValueAvailableAt(const AbstractValue &V,
                                      const ProgramPointSite &Site) const {
  return isAvailable() && mair::isValueAvailableAt(V, Site.getAnchor(), *DT);
}

bool MAIRLoopInfo::executesOncePerRelevantIteration(const ProgramPoint &PP,
                                                    const Loop &L) const {
  if (!isAvailable() || !PP.hasAlwaysGuard() || !PP.getSite().isAnchored())
    return false;

  // An internal irreducible cycle can revisit a dominated program point more
  // than once during one natural-loop iteration. This check belongs to
  // execution counting.
  LoopBlocksRPO RPOT(const_cast<Loop *>(&L));
  RPOT.perform(LI);
  if (containsIrreducibleCFG<const BasicBlock *>(RPOT, *LI))
    return false;

  const BasicBlock *SiteBlock = PP.getBasicBlock();
  BasicBlock *Latch = L.getLoopLatch();
  if (!SiteBlock || !Latch || LI->getLoopFor(SiteBlock) != &L)
    return false;

  return DT->dominates(SiteBlock, Latch);
}

// Both counts are one bit wider than the backedge-taken count, so the trip
// count of a loop whose backedge count is all ones does not wrap.
// A site before the exit test also runs in the iteration that leaves the
// loop: that is the trip count. A site after it runs once per backedge.
static const SCEV *executionsFromBackedgeCount(ScalarEvolution &SE,
                                               const SCEV *BackedgeTakenCount,
                                               bool RunsInLastIteration) {
  if (RunsInLastIteration)
    return SE.getTripCountFromExitCount(BackedgeTakenCount);
  unsigned Width = BackedgeTakenCount->getType()->getIntegerBitWidth();
  return SE.getZeroExtendExpr(
      BackedgeTakenCount,
      IntegerType::get(BackedgeTakenCount->getType()->getContext(), Width + 1));
}

const SCEV *MAIRLoopInfo::getExactExecutionCountSCEV(const ProgramPoint &PP,
                                                     const Loop &L) const {
  if (!executesOncePerRelevantIteration(PP, L))
    return nullptr;

  const SCEV *BackedgeTakenCount = getSE().getBackedgeTakenCount(&L);
  if (isa<SCEVCouldNotCompute>(BackedgeTakenCount) ||
      !BackedgeTakenCount->getType()->isIntegerTy())
    return nullptr;

  BasicBlock *Exiting = L.getExitingBlock();
  if (!Exiting)
    return nullptr;

  return executionsFromBackedgeCount(
      getSE(), BackedgeTakenCount, DT->dominates(PP.getBasicBlock(), Exiting));
}

// Accesses at most this many bytes apart cannot step over an unmapped page.
static constexpr uint64_t MaxFaultingStride = 4096;

// The backedge-taken count of the run of L that just left through its
// exiting block, from the induction variable IV read there as Last:
// Last = Start + BTC * Step, so BTC = (Last - Start) / Step.
//
// The subtraction and division are exact only if IV did not wrap: BTC * |Step|
// must stay below 2^Width. SCEV proves that with a no-self-wrap, no-signed-wrap
// or (for an increasing variable) no-unsigned-wrap flag. Without a flag it
// still holds if IV is at least as wide as an address and steps no further per
// iteration than the accesses, which are at most a page apart: they would have
// swept the whole address space, and so faulted on an unmapped page, before IV
// could wrap.
static const SCEV *backedgeCountFromExitValue(ScalarEvolution &SE, PHINode &IV,
                                              const Loop &L, Value &Last,
                                              const SCEVAddRecExpr &Address,
                                              const DataLayout &DL) {
  const auto *AddRec = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(&IV));
  if (!AddRec || !AddRec->isAffine() || AddRec->getLoop() != &L)
    return nullptr;
  const auto *Step = dyn_cast<SCEVConstant>(AddRec->getStepRecurrence(SE));
  if (!Step || Step->getAPInt().isZero())
    return nullptr;
  const APInt &StepValue = Step->getAPInt();
  APInt AbsoluteStep = StepValue.abs();

  bool NoWrap = AddRec->hasNoSelfWrap() || AddRec->hasNoSignedWrap() ||
                (AddRec->hasNoUnsignedWrap() && StepValue.isStrictlyPositive());
  if (!NoWrap) {
    const auto *AddressStep =
        dyn_cast<SCEVConstant>(Address.getStepRecurrence(SE));
    if (!AddressStep)
      return nullptr;
    APInt AbsoluteAddressStep = AddressStep->getAPInt().abs();
    if (AbsoluteAddressStep.isZero() ||
        AbsoluteAddressStep.ugt(MaxFaultingStride) ||
        AbsoluteStep.getActiveBits() > 64 ||
        AbsoluteStep.getZExtValue() > AbsoluteAddressStep.getZExtValue() ||
        SE.getTypeSizeInBits(IV.getType()) <
            DL.getIndexTypeSizeInBits(Address.getType()))
      return nullptr;
  }

  BasicBlock *Preheader = L.getLoopPreheader();
  Value *Start = Preheader ? IV.getIncomingValueForBlock(Preheader) : nullptr;
  if (!Start)
    return nullptr;
  // Both ends are values in the IR: the expression stays opaque to SCEV, which
  // would otherwise fold Last back into the recurrence.
  const SCEV *First =
      isa<Constant>(Start) ? SE.getSCEV(Start) : SE.getUnknown(Start);
  const SCEV *Final = SE.getUnknown(&Last);
  if (IV.getType()->isPointerTy()) {
    First = SE.getLosslessPtrToIntExpr(First);
    Final = SE.getLosslessPtrToIntExpr(Final);
    if (isa<SCEVCouldNotCompute>(First) || isa<SCEVCouldNotCompute>(Final))
      return nullptr;
  } else if (!IV.getType()->isIntegerTy()) {
    return nullptr;
  }
  const SCEV *Distance = StepValue.isNegative() ? SE.getMinusSCEV(First, Final)
                                                : SE.getMinusSCEV(Final, First);
  if (isa<SCEVCouldNotCompute>(Distance))
    return nullptr;
  if (AbsoluteStep.isOne())
    return Distance;
  unsigned Width = Distance->getType()->getIntegerBitWidth();
  if (AbsoluteStep.getActiveBits() > Width)
    return nullptr;
  return SE.getUDivExpr(Distance,
                        SE.getConstant(AbsoluteStep.zextOrTrunc(Width)));
}

const SCEV *MAIRLoopInfo::getExitValueExecutionCountSCEV(
    const ProgramPoint &PP, const Loop &L, const ProgramPointSite &Site,
    const SCEVAddRecExpr &Address) const {
  if (!executesOncePerRelevantIteration(PP, L))
    return nullptr;
  BasicBlock *Exiting = L.getExitingBlock();
  BasicBlock *Exit = L.getUniqueExitBlock();
  if (!Exiting || !Exit || LI->getLoopFor(Exiting) != &L)
    return nullptr;

  // The induction variables hold the values of the last iteration, the one
  // that took the exit. The source ran in it if it comes before the exit test
  // and did not if it comes after.
  const BasicBlock *Source = PP.getBasicBlock();
  bool RunsInLastIteration = DT->dominates(Source, Exiting);
  if (!RunsInLastIteration && !DT->dominates(Exiting, Source))
    return nullptr;

  // Read the variable through its LCSSA phi if there is one.
  auto ValueAtExit = [&](PHINode &IV) -> Value * {
    for (PHINode &Phi : Exit->phis())
      if (Phi.getNumIncomingValues() == 1 && Phi.getIncomingValue(0) == &IV &&
          isValueAvailableAt(Phi, Site))
        return &Phi;
    return isValueAvailableAt(IV, Site) ? &IV : nullptr;
  };

  // An integer variable gives a simpler count than a pointer one.
  ScalarEvolution &SE = getSE();
  const DataLayout &DL = Func->getDataLayout();
  const SCEV *PointerCount = nullptr;
  for (PHINode &IV : L.getHeader()->phis()) {
    if (!SE.isSCEVable(IV.getType()) ||
        (PointerCount && IV.getType()->isPointerTy()))
      continue;
    Value *Last = ValueAtExit(IV);
    const SCEV *Count =
        Last ? backedgeCountFromExitValue(SE, IV, L, *Last, Address, DL)
             : nullptr;
    if (!Count)
      continue;
    if (IV.getType()->isIntegerTy())
      return executionsFromBackedgeCount(SE, Count, RunsInLastIteration);
    PointerCount = Count;
  }
  return PointerCount ? executionsFromBackedgeCount(SE, PointerCount,
                                                    RunsInLastIteration)
                      : nullptr;
}

bool MAIRLoopInfo::isLoopInvariant(const Value *V, const Loop &L) const {
  return V && L.isLoopInvariant(V);
}

bool MAIRLoopInfo::isLoopInvariant(const AbstractValue &Value,
                                   const Loop &L) const {
  return Value.isConstant() ||
         (Value.isSymbol() && isLoopInvariant(Value.getSymbol(), L));
}

/// Whether every LLVM value \p Expr reads is invariant in \p L.
static bool isExprLoopInvariant(const MAIRScalarExpr &Expr, const Loop &L) {
  if (Expr.getKind() == MAIRScalarExpr::Kind::LLVMValue)
    return L.isLoopInvariant(Expr.getLLVMValue());
  return llvm::all_of(Expr.operands(), [&](const MAIRScalarExpr &Operand) {
    return isExprLoopInvariant(Operand, L);
  });
}

bool MAIRLoopInfo::isLoopInvariant(const Observation &Obs,
                                   const Loop &L) const {
  const AbstractAccess *Loc = Obs.getContiguousRegion();
  // An inner loop's range may have a size expression instead of a size.
  return Loc && isLoopInvariant(Loc->getBase(), L) &&
         isLoopInvariant(Loc->getOffset(), L) &&
         (Loc->hasSizeExpr() ? isExprLoopInvariant(Loc->getSizeExpr(), L)
                             : isLoopInvariant(Loc->getSize(), L));
}

Loop *MAIRLoopInfo::getTopInvariantLoop(const Observation &Obs,
                                        Loop &Start) const {
  if (!isLoopInvariant(Obs, Start))
    return nullptr;

  Loop *Top = &Start;
  while (Loop *Parent = Top->getParentLoop()) {
    if (!isLoopInvariant(Obs, *Parent))
      break;
    Top = Parent;
  }
  return Top;
}

ProgramPoint *MAIRLoopInfo::getPreheaderExit(const Loop &L) const {
  if (!Graph)
    return nullptr;
  MAIRGraph *Body = Graph->getLoopGraph(L);
  return Body ? Body->getLoopPreheader() : nullptr;
}

const BasicBlock *MAIRLoopInfo::getMotionSource(const ProgramPoint &PP,
                                                const Loop &L) const {
  if (!isAvailable() || PP.getParent() != Graph || Graph->getLoop() != &L ||
      !PP.hasAlwaysGuard() || !PP.getSite().isAnchored() ||
      Graph->hasForwardBarrier() || Graph->hasBackwardBarrier())
    return nullptr;
  const BasicBlock *Source = PP.getBasicBlock();
  return Source && L.contains(Source) ? Source : nullptr;
}

// An observation may move to the preheader with an always guard only if every
// entry of the loop executes its source before the loop is left.
//
//   Ho:                                 ; outer header, %p = &base[%j]
//     br label %Hi
//   Hi:                                 ; inner header
//     br i1 (%j > 5), label %S, label %Lo.latch
//   S:
//     store i32 0, ptr %p
//     br i1 %done, label %exit, label %Hi.latch
//   Hi.latch:
//     br label %Hi
//   Lo.latch:                           ; %j.next = %j + 1
//     br label %Ho
//
// S post-dominates the inner preheader Ho: the function returns only from S.
// Still, for %j <= 5 the inner loop is left through Hi without running S, and
// control comes back around the outer backedge with another %p. Observing %p
// in Ho would report base[0..5], which the program never writes.
//
// The condition is therefore that the source block dominates every exiting
// block: then no exit can be taken before the source has run. It implies
// post-dominance of the preheader, because every path from the preheader to a
// return leaves the loop through an exiting block, so post-dominance is not
// checked separately. A loop without exits can only be left by running
// forever, so there the source must dominate every latch instead: each
// iteration runs it. As with post-dominance, an iteration that never ends may
// be observed without having run the source.
//
// TODO: The observation above can still leave the loop. It needs an "S has
// executed" indicator, which the guarded sink provides for a loop with one
// exit. Loops with several exits, like Hi, need a guarded copy on each exit
// edge and the CFG rewrite that goes with it.
bool MAIRLoopInfo::canHoistToPreheader(const ProgramPoint &PP,
                                       const Loop &L) const {
  const BasicBlock *Source = getMotionSource(PP, L);
  if (!Source || !L.getLoopPreheader())
    return false;
  SmallVector<BasicBlock *, 4> IterationEnds;
  L.getExitingBlocks(IterationEnds);
  if (IterationEnds.empty())
    L.getLoopLatches(IterationEnds);
  return all_of(IterationEnds, [&](const BasicBlock *End) {
    return DT->dominates(Source, End);
  });
}

bool MAIRLoopInfo::canSinkToExit(const ProgramPoint &PP, const Loop &L) const {
  const BasicBlock *Source = getMotionSource(PP, L);
  if (!Source)
    return false;
  std::optional<MAIRLoopExit> Exit = describeUniqueExit(L);
  return Exit && PDT->dominates(Exit->Exit, Source);
}

std::optional<MAIRLoopExit>
MAIRLoopInfo::describeUniqueExit(const Loop &L) const {
  BasicBlock *Exit = L.getUniqueExitBlock();
  BasicBlock *Exiting = L.getExitingBlock();
  if (!Exit || !Exiting)
    return std::nullopt;
  return MAIRLoopExit{Exiting, Exit, Exit->getUniquePredecessor() == Exiting};
}

ProgramPoint *MAIRLoopInfo::getUniqueExitEntry(const Loop &L) const {
  if (!Graph)
    return nullptr;
  MAIRGraph *Body = Graph->getLoopGraph(L);
  BasicBlock *Exit = L.getUniqueExitBlock();
  if (!Body || !Exit)
    return nullptr;
  for (LoopProgramPoint *Boundary : Body->getLoopExits())
    if (Boundary->getBasicBlock() == Exit)
      return Boundary;
  return nullptr;
}

std::optional<ProgramPointSite> MAIRLoopInfo::getExitSite(const Loop &L) const {
  std::optional<MAIRLoopExit> Exit = describeUniqueExit(L);
  ProgramPoint *Boundary = getUniqueExitEntry(L);
  if (!Exit || !Boundary)
    return std::nullopt;
  return Exit->IsDedicated
             ? Boundary->getSite()
             : ProgramPointSite::edge(*Exit->Exiting, *Exit->Exit);
}

ProgramPoint *
MAIRLoopInfo::findExitPoint(const Loop &L, const ProgramPointSite &Site,
                            const ProgramPointGuard &Guard) const {
  ProgramPoint *Boundary = getUniqueExitEntry(L);
  if (!Boundary)
    return nullptr;
  auto Fits = [&](const ProgramPoint &PP) {
    return PP.getSite() == Site && PP.getGuard() == Guard;
  };
  // The entry point of the exit block has the boundary's site and guard too.
  if (Fits(*Boundary))
    return Boundary;
  for (ProgramPoint &PP : *Boundary->getParent())
    if (Fits(PP))
      return &PP;
  return nullptr;
}

ScalarEvolution &MAIRLoopInfo::getSE() const {
  if (!FAM || !Func)
    report_fatal_error("MAIR LoopAnalysis requires a FunctionAnalysisManager");
  if (&FAM->getResult<llvm::LoopAnalysis>(*Func) != LI)
    report_fatal_error(
        "MAIR LoopAnalysis requires the MAIRGraph and ScalarEvolution to "
        "share LoopInfo");
  return FAM->getResult<ScalarEvolutionAnalysis>(*Func);
}

AssumptionCache &MAIRLoopInfo::getAssumptionCache() const {
  if (!FAM || !Func)
    report_fatal_error("MAIR LoopAnalysis requires a FunctionAnalysisManager");
  return FAM->getResult<AssumptionAnalysis>(*Func);
}

LoopAnalysis::Result LoopAnalysis::run(MAIRGraph &G,
                                       MAIRGraphAnalysisManager &) {
  if (!G.isLive())
    report_fatal_error("MAIR LoopAnalysis cannot run on a consumed graph");
  // Graphs built without a function analysis manager have no ScalarEvolution.
  // Callers treat that as an unavailable loop analysis.
  if (!FAM)
    return MAIRLoopInfo();
  return MAIRLoopInfo(G, *FAM);
}

} // namespace mair
