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
        if (G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
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
      if (G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
        Worklist.push_back({&Edge.getNode(), Fact.Source});
  }

  return Result;
}

} // namespace mair
