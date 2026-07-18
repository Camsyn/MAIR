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
  if (llvm::any_of(Accesses, [&](const AvailableAccess &Existing) {
        return Existing == Access;
      }))
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
  DenseSet<const MemoryAccess *> ExcludedSources;
  return covers(Access, ExcludedSources);
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
  if (Accesses.size() != Other.Accesses.size())
    return false;
  return llvm::all_of(Accesses, [&](const AvailableAccess &Access) {
    return llvm::any_of(Other.Accesses, [&](const AvailableAccess &Candidate) {
      return Candidate == Access;
    });
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

  bool Changed;
  do {
    Changed = false;
    for (const Observation &Obs : G) {
      AvailableAccessSet In;
      for (const ObservationEdge &Edge : G.parentEdges(Obs))
        if (G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
          In.unionWith(Result.OutSets.find(&Edge.getNode())->second);

      AvailableAccessSet Out = In;
      for (const MemoryAccess &Access : Obs)
        Out.insert(AvailableAccess{&Access, Access.getParent()});

      AvailableAccessSet &OldIn = Result.InSets.find(&Obs)->second;
      AvailableAccessSet &OldOut = Result.OutSets.find(&Obs)->second;
      if (OldIn != In || OldOut != Out) {
        OldIn = std::move(In);
        OldOut = std::move(Out);
        Changed = true;
      }
    }
  } while (Changed);

  return Result;
}

} // namespace mair
