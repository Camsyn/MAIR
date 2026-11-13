#include "MAIR/MAIR.h"

#include "MAIR/MAIRInternal.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CycleInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

namespace {
const Instruction *getBarrierRangeStart(const ProgramPoint &PP) {
  return PP.isBasicBlockEntrySite() ? nullptr : &PP.getAnchor();
}

const Instruction *getBarrierRangeEnd(const ProgramPoint &PP) {
  return &PP.getAnchor();
}

bool isInterestingMop(const Instruction &I) {
  if (I.hasMetadata(LLVMContext::MD_nosanitize))
    return false;
  return isa<LoadInst, StoreInst, AtomicRMWInst, AtomicCmpXchgInst,
             AnyMemIntrinsic>(I) ||
         isMaskedMemoryIntrinsic(I);
}

void verifyNaturalLoops(const LoopInfo &LI) {
  for (const Loop *Loop : LI.getLoopsInPreorder()) {
    if (!Loop->getLoopPreheader())
      report_fatal_error(
          "MAIR requires a loop preheader before graph building");
    if (!Loop->hasDedicatedExits())
      report_fatal_error(
          "MAIR requires dedicated loop exits before graph building");
  }
}

// The outermost cycle of \p From of which another iteration can start before
// control reaches \p To, which post-dominates it; null if there is none.
//
//   H:                                  ; %p = &base[%i]
//     br i1 (%i < 3), label %X, label %B
//   X:
//     br label %A
//   B:
//     store i32 1, ptr %p               ; writes base[i] for i >= 3
//     br i1 %done, label %exit, label %A
//   A:
//     store i32 2, ptr %p               ; writes base[i] in every iteration
//     br label %H
//
// The function returns only from B, so B post-dominates A. Yet for %i < 3
// control goes from A around the backedge to H before it reaches B, and %p is
// another address by then. Deleting A's observation as "covered by B" would
// leave the writes of base[0..2] unobserved.
//
// Post-dominance around a backedge is still as good as any other for an
// address that does not change with the loop. So the edge is kept and records
// the cycle; a pass relates two observations along it only if they are
// invariant in that cycle (ProgramPointEdge::isOpenTo).
//
// LLVM cycles generalize natural loops to irreducible control flow, where a
// cycle has several entries and LLVM picks the one its DFS reaches first as
// the header. One rule then serves loops and irreducible cycles alike. A path
// iterates a cycle C of From when it
//   - enters the header of C, or
//   - leaves C: it can come back only through an entry, and outside of C the
//     values C defined belong to an iteration that is over.
// Every circuit of C that avoids its header lies in a nested cycle, so a path
// with neither event keeps what C defined outside its nested cycles. A path
// that iterates C iterates the cycles of From inside C as well, so the
// outermost one stands for all of them.
//
// Dominance needs no rule. If W dominates T, then after the last definition
// of a value before T control has passed W again, because a path that
// reached T without doing so would bypass W altogether.
//
// TODO: Both stores above could instead move to H, which runs one of them in
// every iteration, and periodic combination could then take the result out
// of the loop. That needs common observation motion to hoist across the
// empty block X.
const Cycle *iteratedCycleBetween(const BasicBlock &From, const BasicBlock &To,
                                  const CycleInfo &Cycles) {
  const Cycle *Innermost = Cycles.getCycle(&From);
  if (!Innermost)
    return nullptr;
  const Cycle *Outermost = Innermost;
  while (const Cycle *Parent = Outermost->getParentCycle())
    Outermost = Parent;

  const Cycle *Iterated = nullptr;
  SmallPtrSet<const BasicBlock *, 16> Visited;
  SmallVector<const BasicBlock *, 16> Worklist{&From};
  while (!Worklist.empty())
    for (const BasicBlock *Succ : successors(Worklist.pop_back_val())) {
      if (!Visited.insert(Succ).second)
        continue;
      // The outermost cycle of From that stepping to Succ iterates.
      const Cycle *Event = nullptr;
      for (const Cycle *C = Innermost; C; C = C->getParentCycle()) {
        if (C->contains(Succ)) {
          if (C->getHeader() == Succ)
            Event = C;
          break;
        }
        Event = C;
      }
      if (Event && (!Iterated || Event->contains(Iterated)))
        Iterated = Event;
      if (Iterated == Outermost)
        return Iterated;
      if (Succ != &To)
        Worklist.push_back(Succ);
    }
  return Iterated;
}

} // namespace

// Build the initial, unoptimized MAIR graph for one LLVM function.
//
// ProgramPoint layout follows each LLVM basic block:
//   BBEntry -> memory-instruction program points in source order -> BBExit.
// Edges are then added for adjacent program points inside each block and for
// immediate dominator/post-dominator relationships across blocks. Barrier
// queries decide whether the corresponding forward/backward sanitizer
// reachability edge is legal.
MAIRGraph MAIRGraph::build(Function &F, const DominatorTree &DT,
                           const PostDominatorTree &PDT, const LoopInfo &LI,
                           MAIRBarrierAnalysisCore &BA,
                           MAIROptimizationConfig Config) {
  verifyNaturalLoops(LI);
  MAIRGraph Graph(F, Config);
  Graph.Analyses.bind(DT, PDT, LI);
  Graph.Analyses.Cycles = std::make_unique<CycleInfo>();
  Graph.Analyses.Cycles->compute(F);
  const CycleInfo &Cycles = *Graph.Analyses.Cycles;
  // Only natural loops get a scope of their own. The blocks of an irreducible
  // cycle stay in the enclosing scope, where the ordinary rules apply to
  // them; post-dominance edges record the cycles they iterate.
  //
  // TODO: An irreducible cycle has no preheader, single header or trip count,
  // so invariant motion and periodic combination do not apply to it. Explore
  // them if such cycles turn out to matter.
  DenseMap<const Loop *, MAIRGraph *> LoopGraphs;
  for (const Loop *Loop : LI.getLoopsInPreorder()) {
    MAIRGraph *Parent = Loop->getParentLoop()
                            ? LoopGraphs.lookup(Loop->getParentLoop())
                            : &Graph;
    auto Body = std::unique_ptr<MAIRGraph>(new MAIRGraph(*Parent, *Loop));
    LoopGraphs[Loop] = Body.get();
    Parent->ChildGraphs.push_back(std::move(Body));
  }
  auto ScopeFor = [&](const BasicBlock &BB) -> MAIRGraph & {
    const Loop *Loop = LI.getLoopFor(&BB);
    return Loop ? *LoopGraphs.lookup(Loop) : Graph;
  };
  using BlockPoints =
      DenseMap<const BasicBlock *, SmallVector<ProgramPoint *, 4>>;
  DenseMap<MAIRGraph *, BlockPoints> OrderedPPs;

  // ===========================================================
  // 1. Create program points for memory instructions and block boundaries
  // ===========================================================
  for (BasicBlock &BB : F) {
    MAIRGraph &Scope = ScopeFor(BB);
    Scope.ForwardBarrier |= BA.hasFwdBarrier(BB);
    Scope.BackwardBarrier |= BA.hasBwdBarrier(BB);
    auto &PPsInOneBB = OrderedPPs[&Scope][&BB];
    PPsInOneBB.push_back(&Scope.createBBProgramPoint(BB, true));
    for (Instruction &I : BB)
      if (isInterestingMop(I))
        PPsInOneBB.push_back(&Scope.createInstProgramPoint(BB, I));
    PPsInOneBB.push_back(&Scope.createBBProgramPoint(BB, false));
  }
  for (const Loop *Loop : LI.getLoopsInPreorder()) {
    MAIRGraph &Body = *LoopGraphs.lookup(Loop);
    MAIRGraph &Parent = *Body.Parent;
    BasicBlock *Preheader = Loop->getLoopPreheader();
    Body.LoopPreheader = &Parent.createLoopProgramPoint(
        ProgramPointSite::anchored(*Preheader->getTerminator(),
                                   ProgramPointSite::Kind::BasicBlockExit),
        LoopProgramPoint::BoundaryKind::LoopPreheader, Body);
    OrderedPPs[&Parent][Preheader].push_back(Body.LoopPreheader);
    SmallVector<BasicBlock *, 4> ExitBlocks;
    Loop->getUniqueExitBlocks(ExitBlocks);
    for (BasicBlock *Exit : ExitBlocks) {
      auto &Boundary = Parent.createLoopProgramPoint(
          ProgramPointSite::anchored(*Exit->getFirstNonPHIIt(),
                                     ProgramPointSite::Kind::BasicBlockEntry),
          LoopProgramPoint::BoundaryKind::LoopExit, Body);
      Body.LoopExits.push_back(&Boundary);
      auto &Points = OrderedPPs[&Parent][Exit];
      Points.insert(Points.begin(), &Boundary);
    }
  }

  // Propagate the barrier status from children to parents.
  Graph.propagateBarrier();

  // ===========================================================
  // 2. Connect program point edges inside each block
  // ===========================================================
  for (auto &[Scope, BlockPPs] : OrderedPPs) {
    for (auto &[BB, Points] : BlockPPs) {
      for (unsigned I = 1; I < Points.size(); ++I) {
        ProgramPoint &Prev = *Points[I - 1], &Next = *Points[I];
        const Instruction *PrevI = getBarrierRangeStart(Prev);
        const Instruction *NextI = getBarrierRangeEnd(Next);
        bool FwdEndpointBarrier =
            Prev.isMemoryInstSite() && BA.isFwdBarrier(Prev.getAnchor());
        bool BwdEndpointBarrier =
            Prev.isMemoryInstSite() && BA.isBwdBarrier(Prev.getAnchor());
        const bool SameSite = Prev.getSite() == Next.getSite();
        if (SameSite || (!FwdEndpointBarrier &&
                         !BA.hasFwdBarrierBetween(*BB, PrevI, NextI)))
          Scope->addEdge(Prev, Next, PPEdgeKind::Dom);
        if (SameSite || (!BwdEndpointBarrier &&
                         !BA.hasBwdBarrierBetween(*BB, PrevI, NextI)))
          Scope->addEdge(Next, Prev, PPEdgeKind::PostDom);
      }
    }
  }

  // ===========================================================
  // 3. Connect program point edges across blocks via DT/PDT
  // ===========================================================
  auto GetIDom = [](auto &DT, const BasicBlock &BB) -> const BasicBlock * {
    if (const DomTreeNode *Node = DT.getNode(&BB))
      if (const DomTreeNode *IDom = Node->getIDom())
        return IDom->getBlock();
    return nullptr;
  };
  // The nearest strict (post-)dominator of BB in the scope of BB.
  auto GetIDomInScope = [&](auto &Tree,
                            const BasicBlock &BB) -> const BasicBlock * {
    const BasicBlock *IDom = GetIDom(Tree, BB);
    while (IDom && &ScopeFor(*IDom) != &ScopeFor(BB))
      IDom = GetIDom(Tree, *IDom);
    return IDom;
  };
  for (BasicBlock &BB : F) {
    MAIRGraph &Scope = ScopeFor(BB);
    BlockPoints &ScopePPs = OrderedPPs[&Scope];
    if (const BasicBlock *IDom = GetIDomInScope(DT, BB))
      if (!BA.isFwdBarrier(*IDom->getTerminator()) &&
          !BA.hasFwdBarrierPath(*IDom, BB))
        Scope.addEdge(*ScopePPs[IDom].back(), *ScopePPs[&BB].front(),
                      PPEdgeKind::Dom);
    if (const BasicBlock *IPDom = GetIDomInScope(PDT, BB))
      if (!BA.isBwdBarrier(*BB.getTerminator()) &&
          !BA.hasBwdBarrierPath(BB, *IPDom))
        Scope.addEdge(*ScopePPs[IPDom].front(), *ScopePPs[&BB].back(),
                      PPEdgeKind::PostDom,
                      iteratedCycleBetween(BB, *IPDom, Cycles));
  }

  return Graph;
}

} // namespace mair
