#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace mair {

AnalysisKey RedundantObservationAnalysis::Key;

bool AvailableAccessSet::insert(const MemoryAccess &Access) {
  return Sources.insert(&Access);
}

bool AvailableAccessSet::covers(
    const MemoryAccess &Access,
    const DenseSet<const MemoryAccess *> &ExcludedSources) const {
  return llvm::any_of(Sources, [&](const MemoryAccess *Source) {
    return !ExcludedSources.contains(Source) &&
           Source->getParent() != Access.getParent() && Source->covers(Access);
  });
}

const AvailableAccessSet &
RedundantObservationInfo::getIn(const Observation &Obs) const {
  return InSets.find(&Obs)->second;
}

namespace {

struct PendingFact {
  const Observation *Destination = nullptr;
  const MemoryAccess *Source = nullptr;
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
// A PostDom edge may connect those observations. They are redundant only when
// both are in the same loop. Two blocks outside every loop also match.
bool sameLoopForPostDom(const Observation &From, const Observation &To,
                        const llvm::LoopInfo &LI) {
  const BasicBlock *FromBB = From.getBasicBlock();
  const BasicBlock *ToBB = To.getBasicBlock();
  if (!FromBB || !ToBB)
    return false;
  return LI.getLoopFor(FromBB) == LI.getLoopFor(ToBB);
}

bool canPropagate(const MAIRGraph &G, const Observation &From,
                  const ObservationEdge &Edge) {
  if (!G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
    return false;
  // Some unit-test graphs are built without analyses. A graph that has no
  // LoopInfo keeps the previous coverage rule.
  if (Edge.getKind() == ObsEdgeKind::PostDom && G.hasLoopInfo() &&
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
  for (const Observation &Obs : G) {
    Result.InSets.try_emplace(&Obs);
    for (const MemoryAccess &Access : Obs) {
      if (!Access.isDeletable())
        continue;
      // A local access becomes available only after its observation, so seed
      // it at each allowed child rather than in the source observation's IN.
      for (const ObservationEdge &Edge : Obs.children())
        if (canPropagate(G, Obs, Edge))
          Worklist.push_back({&Edge.getNode(), &Access});
    }
  }

  // Propagate only the newly discovered (observation, access) fact. The set
  // insertion is both the visited check and the fixed-point condition: cycles
  // terminate because an identical fact is never forwarded twice.
  while (!Worklist.empty()) {
    PendingFact Fact = Worklist.pop_back_val();
    AvailableAccessSet &In = Result.InSets.find(Fact.Destination)->second;
    if (!In.insert(*Fact.Source))
      continue;

    // Edge kinds are filtered at every hop because sanitizer configurations
    // may allow Dom coverage while rejecting PostDom coverage.
    for (const ObservationEdge &Edge : Fact.Destination->children())
      if (canPropagate(G, *Fact.Destination, Edge))
        Worklist.push_back({&Edge.getNode(), Fact.Source});
  }

  return Result;
}

} // namespace mair
