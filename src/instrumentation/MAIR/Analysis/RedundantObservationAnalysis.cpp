#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace mair {

AnalysisKey RedundantObservationAnalysis::Key;

namespace {

bool kindCovers(AccessKind Available, AccessKind Required) {
  return (static_cast<unsigned>(Available) & static_cast<unsigned>(Required)) ==
         static_cast<unsigned>(Required);
}

std::optional<AvailableAccess> extract(const MemoryAccess &Access) {
  const AbstractAccess *Region = Access.getContiguousRegion();
  if (!Region)
    return std::nullopt;
  return AvailableAccess{&Access, Access.getParent(), &Access.getMemoryObject(),
                         *Region, Access.getKind()};
}

} // namespace

bool AvailableAccess::covers(const MemoryAccess &Other) const {
  const AbstractAccess *OtherRegion = Other.getContiguousRegion();
  if (!OtherRegion)
    return false;
  return SourceObservation != Other.getParent() &&
         Object == &Other.getMemoryObject() && Access.contains(*OtherRegion) &&
         kindCovers(Kind, Other.getKind());
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
      for (const Observation *Pred : G.predecessors(Obs))
        In.unionWith(Result.OutSets.find(Pred)->second);

      AvailableAccessSet Out = In;
      for (const MemoryAccess &Access : Obs)
        if (std::optional<AvailableAccess> Available = extract(Access))
          Out.insert(*Available);

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
