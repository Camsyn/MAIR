#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

#include <array>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "mair-redundant-observation-elimination"

STATISTIC(NumEliminatedAccesses, "Number of redundant accesses eliminated");
STATISTIC(
    NumInputAccesses,
    "Number of memory accesses entering redundant observation elimination");

namespace mair {

namespace {

struct ConcreteRange {
  uint64_t Begin = 0;
  uint64_t End = 0;
};

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

std::optional<ConcreteRange> getConcreteRange(const MemoryAccess &Access) {
  const AbstractAccess *Region = Access.getContiguousRegion();
  if (!Region || !Region->getOffset().isConstant() ||
      !Region->getSize().isConstant())
    return std::nullopt;

  uint64_t Begin = Region->getOffset().getConstant();
  uint64_t Size = Region->getSize().getConstant();
  if (Size > std::numeric_limits<uint64_t>::max() - Begin)
    return std::nullopt;
  return ConcreteRange{Begin, Begin + Size};
}

bool compareForCoverage(const MemoryAccess &LHS, const MemoryAccess &RHS) {
  if (LHS.getBase() != RHS.getBase())
    return std::less<const Value *>()(LHS.getBase(), RHS.getBase());

  std::optional<ConcreteRange> LRange = getConcreteRange(LHS);
  std::optional<ConcreteRange> RRange = getConcreteRange(RHS);
  if (LRange.has_value() != RRange.has_value())
    return LRange.has_value();
  if (!LRange)
    return false;
  if (LRange->Begin != RRange->Begin)
    return LRange->Begin < RRange->Begin;
  if (LRange->End != RRange->End)
    return LRange->End > RRange->End;
  return LHS.getLevel() > RHS.getLevel();
}

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
  Obs.sort(compareForCoverage);

  // Other region forms need their own coverage index; this sweep is exact only
  // for concrete contiguous half-open intervals.
  for (auto Begin = Obs.begin(); Begin != Obs.end();) {
    std::optional<ConcreteRange> FirstRange = getConcreteRange(*Begin);
    if (!FirstRange) {
      ++Begin;
      continue;
    }

    auto End = std::next(Begin);
    while (End != Obs.end() && End->getBase() == Begin->getBase() &&
           getConcreteRange(*End))
      ++End;

    WitnessesByAtomicity AllWitnesses{};
    WitnessesByAtomicity MergeableWitnesses{};
    // Equal atomicity is directly comparable.  Cross-atomic coverage requires
    // both accesses to be mergeable, hence the second witness table.
    for (auto It = Begin; It != End; ++It) {
      std::optional<ConcreteRange> Range = getConcreteRange(*It);
      assert(Range && "concrete coverage run contains a non-concrete access");
      ConcreteAccess Candidate{&*It, Range->Begin, Range->End};
      MemoryAccess &Access = *It;
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
    Begin = End;
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

  for (MemoryAccess *Access : Redundant)
    Access->getParent()->erase(*Access);
  NumEliminatedAccesses += Redundant.size();
  return Redundant.empty() ? PreservedAnalyses::all()
                           : PreservedAnalyses::none();
}

} // namespace mair
