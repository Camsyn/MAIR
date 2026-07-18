#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

#include <array>
#include <limits>

using namespace llvm;

#define DEBUG_TYPE "mair-redundant-observation-elimination"

STATISTIC(NumEliminatedAccesses, "Number of redundant accesses eliminated");
STATISTIC(
    NumInputAccesses,
    "Number of memory accesses entering redundant observation elimination");

namespace mair {

namespace {

struct ConcreteAccess {
  MemoryAccess *Access = nullptr;
  uint64_t Begin = 0;
  uint64_t End = 0;
};

struct CoveringWitness {
  MemoryAccess *Access = nullptr;
  uint64_t End = 0;
};

using WitnessesByLevel = std::array<CoveringWitness, 256>;
using WitnessesByAtomicity = std::array<WitnessesByLevel, 2>;

bool hasCoveringWitness(const WitnessesByLevel &Witnesses,
                        const ConcreteAccess &Target) {
  // The sweep guarantees that every witness starts no later than Target.  The
  // furthest end at each covering level is therefore the only candidate needed.
  for (unsigned Level = Target.Access->getLevel(); Level < Witnesses.size();
       ++Level) {
    const CoveringWitness &Witness = Witnesses[Level];
    if (Witness.Access && Witness.End >= Target.End &&
        Witness.Access->covers(*Target.Access))
      return true;
  }
  return false;
}

void updateWitness(WitnessesByLevel &Witnesses,
                   const ConcreteAccess &Candidate) {
  CoveringWitness &Witness = Witnesses[Candidate.Access->getLevel()];
  if (!Witness.Access || Candidate.End > Witness.End)
    Witness = {Candidate.Access, Candidate.End};
}

void findIntraObservationRedundancy(
    Observation &Obs, DenseSet<const MemoryAccess *> &DeletedSources,
    SmallVectorImpl<MemoryAccess *> &Redundant) {
  DenseMap<const Value *, SmallVector<ConcreteAccess, 8>> Groups;
  // Other region forms need their own coverage index; this sweep is exact only
  // for concrete contiguous half-open intervals.
  for (MemoryAccess &Access : Obs) {
    const AbstractAccess *Region = Access.getContiguousRegion();
    if (!Region || !Region->getOffset().isConstant() ||
        !Region->getSize().isConstant())
      continue;

    const uint64_t Begin = Region->getOffset().getConstant();
    const uint64_t Size = Region->getSize().getConstant();
    if (Size > std::numeric_limits<uint64_t>::max() - Begin)
      continue;
    Groups[Access.getBase()].push_back({&Access, Begin, Begin + Size});
  }

  for (auto &Entry : Groups) {
    SmallVectorImpl<ConcreteAccess> &Accesses = Entry.second;
    // Earlier starts dominate later starts.  For equal starts, wider and then
    // higher-level accesses must become witnesses before their targets.
    llvm::stable_sort(Accesses,
                      [](const ConcreteAccess &LHS, const ConcreteAccess &RHS) {
                        if (LHS.Begin != RHS.Begin)
                          return LHS.Begin < RHS.Begin;
                        if (LHS.End != RHS.End)
                          return LHS.End > RHS.End;
                        return LHS.Access->getLevel() > RHS.Access->getLevel();
                      });

    WitnessesByAtomicity AllWitnesses{};
    WitnessesByAtomicity MergeableWitnesses{};
    // Equal atomicity is directly comparable.  Cross-atomic coverage requires
    // both accesses to be mergeable, hence the second witness table.
    for (const ConcreteAccess &Candidate : Accesses) {
      MemoryAccess &Access = *Candidate.Access;
      const unsigned Atomicity = Access.isAtomic();
      bool Covered = hasCoveringWitness(AllWitnesses[Atomicity], Candidate);
      if (!Covered && Access.isMergeable())
        Covered = hasCoveringWitness(MergeableWitnesses[!Atomicity], Candidate);

      if (Covered) {
        DeletedSources.insert(&Access);
        Redundant.push_back(&Access);
        continue;
      }
      if (!Access.isDeletable())
        continue;

      updateWitness(AllWitnesses[Atomicity], Candidate);
      if (Access.isMergeable())
        updateWitness(MergeableWitnesses[Atomicity], Candidate);
    }
  }
}

} // namespace

PreservedAnalyses
RedundantObservationEliminationPass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputAccesses += MAIRStat::compute(G).getNumMemoryAccesses();
#endif
  DenseSet<const MemoryAccess *> DeletedSources;
  SmallVector<MemoryAccess *, 16> Redundant;
  // Select from one live snapshot.  Intra-observation deletions cannot witness
  // inter-observation deletions; graph mutation starts only after both phases.
  for (Observation &Obs : G)
    findIntraObservationRedundancy(Obs, DeletedSources, Redundant);

  const RedundantObservationInfo &Info =
      AM.getResult<RedundantObservationAnalysis>(G);
  for (Observation &Obs : G) {
    const AvailableAccessSet &In = Info.getIn(Obs);
    for (MemoryAccess &Access : Obs) {
      if (DeletedSources.contains(&Access))
        continue;
      if (!In.covers(Access, DeletedSources))
        continue;
      DeletedSources.insert(&Access);
      Redundant.push_back(&Access);
    }
  }

  for (MemoryAccess *Access : Redundant) {
    Observation *Parent = Access->getParent();
    auto It = llvm::find_if(Parent->accesses(), [&](MemoryAccess &Candidate) {
      return &Candidate == Access;
    });
    assert(It != Parent->end() && "redundant access must belong to parent");
    Parent->erase(It);
  }
  NumEliminatedAccesses += Redundant.size();
  return Redundant.empty() ? PreservedAnalyses::all()
                           : PreservedAnalyses::none();
}

} // namespace mair
