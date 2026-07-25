#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"

#include <optional>

using namespace llvm;

#define DEBUG_TYPE "mair-simplifygraph"

STATISTIC(NumInputObservations,
          "Number of observations entering graph simplification");
STATISTIC(NumMergedObservations,
          "Number of observations merged by graph simplification");

namespace mair {

namespace {

struct MergeCandidate {
  Observation *Into = nullptr;
  Observation *From = nullptr;
};

bool hasPinnedAccess(const Observation &Obs) {
  return llvm::any_of(
      Obs, [&](const MemoryAccess &Access) { return !Access.isMovable(); });
}

// Equal endpoint loop membership is insufficient when a nested loop lies
// between them, so inspect every CFG path prefix up to the target block.
bool crossesLoopBoundary(const BasicBlock &From, const BasicBlock &To,
                         const LoopInfo &Loops) {
  SmallPtrSet<const BasicBlock *, 16> Visited;
  SmallVector<const BasicBlock *, 16> Worklist{&From};
  Visited.insert(&From);
  while (!Worklist.empty()) {
    const BasicBlock *Current = Worklist.pop_back_val();
    if (Current == &To)
      continue;
    const Loop *CurrentLoop = Loops.getLoopFor(Current);
    for (const BasicBlock *Next : successors(Current)) {
      if (Loops.getLoopFor(Next) != CurrentLoop)
        return true;
      if (Visited.insert(Next).second)
        Worklist.push_back(Next);
    }
  }
  return false;
}

MergeCandidate findMergeCandidate(MAIRGraph &G,
                                  std::optional<LoopInfo> &Loops) {
  for (Observation &O1 : G) {
    if (hasPinnedAccess(O1))
      continue;
    for (const ObservationEdge &Edge : O1.children()) {
      if (!Edge.isDom())
        continue;

      Observation &O2 = Edge.getNode();
      if (&O1 == &O2)
        continue;
      if (hasPinnedAccess(O2))
        continue;
      if (!O1.hasCompatibleMergeSemantics(O2))
        continue;
      if (!O2.hasChild(O1, ObsEdgeKind::PostDom))
        continue;
      const BasicBlock *O1Block = O1.getBasicBlock();
      const BasicBlock *O2Block = O2.getBasicBlock();
      if (O1Block != O2Block) {
        assert(O1Block && O2Block &&
               "compatible observations must use the same site category");
        if (!Loops)
          Loops.emplace(G.getDominatorTree());
        if (crossesLoopBoundary(*O1Block, *O2Block, *Loops))
          continue;
      }
      // Retain the post-dominating site so contraction sinks accesses.
      return {&O2, &O1};
    }
  }
  return {};
}

} // namespace

PreservedAnalyses SimplifyGraphPass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputObservations += MAIRStat::compute(G).getNumObservations();
#else
  (void)AM;
#endif
  bool Changed = false;
  std::optional<LoopInfo> Loops;

  // Contract one mutual Dom/PostDom pair at a time. Rewiring can expose a new
  // pair outside the previous neighborhood, so each contraction starts a fresh
  // topology scan and preserves deterministic list-order selection.
  while (true) {
    MergeCandidate Candidate = findMergeCandidate(G, Loops);
    if (!Candidate.Into)
      break;
    G.contractObservation(*Candidate.Into, *Candidate.From);
    ++NumMergedObservations;
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
