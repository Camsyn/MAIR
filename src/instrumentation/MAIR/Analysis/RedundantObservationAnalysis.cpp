#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace mair {

AnalysisKey RedundantObservationAnalysis::Key;

bool AvailableAccess::covers(const MemoryAccess &Other) const {
  return SourceObservation != Other.getParent() && Source->covers(Other);
}

bool AvailableAccess::operator==(const AvailableAccess &Other) const {
  return Source == Other.Source;
}

bool AvailableAccessSet::insert(const AvailableAccess &Access) {
  assert(Access.Source && "available access must have a source");
  if (!Sources.insert(Access.Source).second)
    return false;
  Accesses.push_back(Access);
  return true;
}

bool AvailableAccessSet::unionWith(const AvailableAccessSet &Other) {
  bool Changed = false;
  for (const AvailableAccess &Access : Other.Accesses)
    Changed |= insert(Access);
  return Changed;
}

bool AvailableAccessSet::covers(const MemoryAccess &Access) const {
  return llvm::any_of(Accesses, [&](const AvailableAccess &Available) {
    return Available.covers(Access);
  });
}

bool AvailableAccessSet::covers(
    const MemoryAccess &Access,
    const DenseSet<const MemoryAccess *> &ExcludedSources) const {
  return llvm::any_of(Accesses, [&](const AvailableAccess &Available) {
    return !ExcludedSources.contains(Available.Source) &&
           Available.covers(Access);
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

    AvailableAccessSet In;
    for (const ObservationEdge &Edge : Obs.parents())
      if (G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
        In.unionWith(Result.OutSets.find(&Edge.getNode())->second);

    AvailableAccessSet Out = In;
    for (const MemoryAccess &Access : Obs)
      Out.insert(AvailableAccess{&Access, Access.getParent()});

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
