#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace mair {

AnalysisKey RedundantObservationAnalysis::Key;

bool AvailableObservationSet::insert(const Observation &Obs) {
  return Sources.insert(&Obs);
}

bool AvailableObservationSet::covers(
    const Observation &Obs,
    const DenseSet<const Observation *> &ExcludedSources) const {
  return llvm::any_of(Sources, [&](const Observation *Source) {
    return !ExcludedSources.contains(Source) &&
           Source->getParent() != Obs.getParent() && Source->covers(Obs);
  });
}

const AvailableObservationSet &
RedundantObservationInfo::getIn(const ProgramPoint &PP) const {
  return InSets.find(&PP)->second;
}

namespace {

struct PendingFact {
  const ProgramPoint *Destination = nullptr;
  const Observation *Source = nullptr;
};

// PostDom coverage compares two uses of a value that dominates both. The
// value can denote different addresses when its definition and the earlier
// use are in one loop and the later use is outside that loop:
//
//   loop:
//     %p = getelementptr i32, ptr %base, i64 %i
//     %v = load i32, ptr %p        ; iteration 0 reads base[0]
//   exit:
//     %last = load i32, ptr %p     ; reads base[n-1]
//
// A PostDom edge may connect their program points. The observations are
// redundant only when both points are in the same loop. Two blocks outside
// every loop also match.
bool sameLoopForPostDom(const ProgramPoint &From, const ProgramPoint &To,
                        const llvm::LoopInfo &LI) {
  const BasicBlock *FromBB = From.getBasicBlock();
  const BasicBlock *ToBB = To.getBasicBlock();
  if (!FromBB || !ToBB)
    return false;
  return LI.getLoopFor(FromBB) == LI.getLoopFor(ToBB);
}

bool canPropagate(const MAIRGraph &G, const ProgramPoint &From,
                  const ProgramPointEdge &Edge) {
  if (!G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
    return false;
  // Some unit-test graphs are built without analyses. A graph that has no
  // LoopInfo keeps the previous coverage rule.
  if (Edge.getKind() == PPEdgeKind::PostDom && G.hasLoopInfo() &&
      !sameLoopForPostDom(From, Edge.getNode(), G.getLoopInfo()))
    return false;
  return true;
}

} // namespace

RedundantObservationInfo
RedundantObservationAnalysis::run(MAIRGraph &G, MAIRGraphAnalysisManager &AM) {
  (void)AM;
  RedundantObservationInfo Result;
  SmallVector<PendingFact, 32> Worklist;
  for (const ProgramPoint &PP : G) {
    Result.InSets.try_emplace(&PP);
    if (!PP.hasAlwaysGuard())
      continue;
    for (const Observation &Obs : PP) {
      // A local observation becomes available only after its program point,
      // so seed it at each allowed child rather than in the source point's IN.
      for (const ProgramPointEdge &Edge : PP.children())
        if (canPropagate(G, PP, Edge))
          Worklist.push_back({&Edge.getNode(), &Obs});
    }
  }

  // Propagate only the newly discovered (program point, observation) fact.
  // Set insertion is both the visited check and the fixed-point condition:
  // cycles terminate because an identical fact is never forwarded twice.
  while (!Worklist.empty()) {
    PendingFact Fact = Worklist.pop_back_val();
    AvailableObservationSet &In = Result.InSets.find(Fact.Destination)->second;
    if (!In.insert(*Fact.Source))
      continue;

    // Edge kinds are filtered at every hop because sanitizer configurations
    // may allow Dom coverage while rejecting PostDom coverage.
    for (const ProgramPointEdge &Edge : Fact.Destination->children())
      if (canPropagate(G, *Fact.Destination, Edge))
        Worklist.push_back({&Edge.getNode(), Fact.Source});
  }

  return Result;
}

} // namespace mair
