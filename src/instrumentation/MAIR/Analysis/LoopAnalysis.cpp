#include "MAIR/Analysis/LoopAnalysis.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopIterator.h"
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

const SCEV *MAIRLoopInfo::getExactExecutionCountSCEV(const ProgramPoint &PP,
                                                     const Loop &L) const {
  if (!executesOncePerRelevantIteration(PP, L))
    return nullptr;

  const SCEV *BackedgeTakenCount = getSE().getBackedgeTakenCount(&L);
  if (isa<SCEVCouldNotCompute>(BackedgeTakenCount) ||
      !BackedgeTakenCount->getType()->isIntegerTy())
    return nullptr;

  BasicBlock *Exiting = L.getExitingBlock();
  const BasicBlock *SiteBlock = PP.getBasicBlock();
  if (!Exiting || !SiteBlock)
    return nullptr;

  // Both counts are one bit wider than the backedge-taken count, so the trip
  // count of a loop whose backedge count is all ones does not wrap.
  // A site before the exit test also runs in the iteration that leaves the
  // loop: that is the trip count. A site after it runs once per backedge.
  if (DT->dominates(SiteBlock, Exiting))
    return getSE().getTripCountFromExitCount(BackedgeTakenCount);
  unsigned Width =
      cast<IntegerType>(BackedgeTakenCount->getType())->getBitWidth();
  return getSE().getZeroExtendExpr(
      BackedgeTakenCount,
      IntegerType::get(L.getHeader()->getContext(), Width + 1));
}

bool MAIRLoopInfo::isLoopInvariant(const Value *V, const Loop &L) const {
  return V && L.isLoopInvariant(V);
}

bool MAIRLoopInfo::isLoopInvariant(const AbstractValue &Value,
                                   const Loop &L) const {
  return Value.isConstant() ||
         (Value.isSymbol() && isLoopInvariant(Value.getSymbol(), L));
}

bool MAIRLoopInfo::isLoopInvariant(const Observation &Obs,
                                   const Loop &L) const {
  const AbstractAccess *Loc = Obs.getContiguousRegion();
  return Loc && isLoopInvariant(Loc->getBase(), L) &&
         isLoopInvariant(Loc->getOffset(), L) &&
         isLoopInvariant(Loc->getSize(), L);
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
