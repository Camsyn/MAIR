#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace mair {

AnalysisKey RedundantObservationAnalysis::Key;

bool AvailableAccess::covers(const MemoryAccess &Other) const {
  assert(Source && "available access must have a source");
  return Source->getParent() != Other.getParent() && Source->covers(Other);
}

bool AvailableAccessSet::insert(const AvailableAccess &Access) {
  assert(Access.Source && "available access must have a source");
  if (!Sources.insert(Access.Source))
    return false;
  return true;
}

bool AvailableAccessSet::unionWith(const AvailableAccessSet &Other) {
  bool Changed = false;
  for (const MemoryAccess *Source : Other.Sources)
    Changed |= insert(AvailableAccess{Source});
  return Changed;
}

bool AvailableAccessSet::covers(const MemoryAccess &Access) const {
  return llvm::any_of(Sources, [&](const MemoryAccess *Source) {
    return AvailableAccess{Source}.covers(Access);
  });
}

bool AvailableAccessSet::covers(
    const MemoryAccess &Access,
    const DenseSet<const MemoryAccess *> &ExcludedSources) const {
  return llvm::any_of(Sources, [&](const MemoryAccess *Source) {
    return !ExcludedSources.contains(Source) &&
           AvailableAccess{Source}.covers(Access);
  });
}

bool AvailableAccessSet::operator==(const AvailableAccessSet &Other) const {
  if (Sources.size() != Other.Sources.size())
    return false;
  return llvm::all_of(Sources, [&](const MemoryAccess *Source) {
    return Other.Sources.contains(Source);
  });
}

const AvailableAccessSet &
RedundantObservationInfo::getIn(const Observation &Obs) const {
  return InSets.find(&Obs)->second;
}

const AvailableAccessSet &
RedundantObservationInfo::getOut(const Observation &Obs) const {
  return OutSets.find(&Obs)->second;
}

namespace {

AvailableAccessSet
computeInSet(const MAIRGraph &G, const Observation &Obs,
             const DenseMap<const Observation *, AvailableAccessSet> &OutSets) {
  AvailableAccessSet In;
  for (const ObservationEdge &Edge : Obs.parents())
    if (G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
      In.unionWith(OutSets.find(&Edge.getNode())->second);
  return In;
}

AvailableAccessSet computeOutSet(const Observation &Obs,
                                 AvailableAccessSet In) {
  for (const MemoryAccess &Access : Obs)
    In.insert(AvailableAccess{&Access});
  return In;
}

} // namespace

RedundantObservationInfo
RedundantObservationAnalysis::run(MAIRGraph &G, MAIRGraphAnalysisManager &AM) {
  (void)AM;
  RedundantObservationInfo Result;
  for (const Observation &Obs : G) {
    Result.InSets.try_emplace(&Obs);
    Result.OutSets.try_emplace(&Obs);
  }

  SmallVector<const Observation *, 16> Worklist;
  DenseSet<const Observation *> Queued;
  auto Enqueue = [&](const Observation &Obs) {
    if (Queued.insert(&Obs).second)
      Worklist.push_back(&Obs);
  };
  for (const Observation &Obs : G)
    Enqueue(Obs);

  // Facts only grow. Recompute a node when a predecessor's OUT set changes,
  // then propagate only when this node's OUT set changes in turn.
  while (!Worklist.empty()) {
    const Observation &Obs = *Worklist.pop_back_val();
    Queued.erase(&Obs);

    AvailableAccessSet In = computeInSet(G, Obs, Result.OutSets);
    AvailableAccessSet Out = computeOutSet(Obs, In);

    AvailableAccessSet &OldIn = Result.InSets.find(&Obs)->second;
    AvailableAccessSet &OldOut = Result.OutSets.find(&Obs)->second;
    const bool OutChanged = OldOut != Out;
    OldIn = std::move(In);
    if (!OutChanged)
      continue;
    OldOut = std::move(Out);

    for (const ObservationEdge &Edge : Obs.children())
      if (G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
        Enqueue(Edge.getNode());
  }

  return Result;
}

} // namespace mair
