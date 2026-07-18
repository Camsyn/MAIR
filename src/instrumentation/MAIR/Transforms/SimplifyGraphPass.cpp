#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/SmallVector.h"
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
      if (hasPinnedAccess(O2))
        continue;
      if (!O1.hasCompatibleMergeSemantics(O2))
        continue;
      if (G.hasEdge(O2, O1, ObsEdgeKind::PostDom))
        // Retain the post-dominating site so contraction sinks accesses.
        return {&O2, &O1};
    }
  }
  return {};
}

void mergeObservationInto(MAIRGraph &G, Observation &Into, Observation &From) {
  const std::optional<ObsEdgeKind> IntoToFrom = Into.hasChild(From);
  const std::optional<ObsEdgeKind> FromToInto = From.hasChild(Into);
  assert(IntoToFrom.has_value() && FromToInto.has_value() &&
         "merged observations must be mutual children");

  Into.prependAccessesFrom(From);

  SmallVector<ObservationEdge, 8> Incoming = G.parentEdges(From);
  SmallVector<ObservationEdge, 8> Outgoing = G.childEdges(From);

  for (const ObservationEdge &Edge : Incoming) {
    Observation &Pred = Edge.getNode();
    if (&Pred != &Into)
      G.addEdge(Pred, Into, Edge.getKind());
  }

  for (const ObservationEdge &Edge : Outgoing) {
    Observation &Succ = Edge.getNode();
    if (&Succ != &Into)
      G.addEdge(Into, Succ, Edge.getKind());
  }

  G.replaceObservation(From, Into);
  G.erase(From);
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

  while (true) {
    MergeCandidate Candidate = findMergeCandidate(G);
    if (!Candidate.Into)
      break;
    mergeObservationInto(G, *Candidate.Into, *Candidate.From);
    ++NumMergedObservations;
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
