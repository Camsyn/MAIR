#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/Statistic.h"
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

MergeCandidate findMergeCandidate(MAIRGraph &G) {
  for (Observation &O1 : G) {
    if (hasPinnedAccess(O1))
      continue;
    for (const ObservationEdge &Edge : O1.children()) {
      if (!Edge.isDom())
        continue;

      Observation &O2 = Edge.getNode();
      if (&O1 == &O2)
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
        const LoopInfo &Loops = G.getLoopInfo();
        if (Loops.getLoopFor(O1Block) != Loops.getLoopFor(O2Block))
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

  // Contract one mutual Dom/PostDom pair at a time. Rewiring can expose a new
  // pair outside the previous neighborhood, so each contraction starts a fresh
  // topology scan and preserves deterministic list-order selection.
  while (true) {
    MergeCandidate Candidate = findMergeCandidate(G);
    if (!Candidate.Into)
      break;
    G.contractObservation(*Candidate.Into, *Candidate.From);
    ++NumMergedObservations;
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
