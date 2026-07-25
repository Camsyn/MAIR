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
STATISTIC(NumEliminatedIntraAccesses,
          "Number of intra-observation redundant accesses eliminated");
STATISTIC(NumEliminatedInterAccesses,
          "Number of inter-observation redundant accesses eliminated");
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

  // A coverage sweep needs containing intervals before the intervals they may
  // eliminate: begin ascending, then end and coverage level descending.
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

unsigned eraseAccesses(SmallVectorImpl<MemoryAccess *> &Accesses) {
  for (MemoryAccess *Access : Accesses)
    Access->eraseFromParent();
  return Accesses.size();
}

unsigned doIntraROE(MAIRGraph &G) {
  SmallVector<MemoryAccess *, 16> Redundant;

  // Phase 1 considers only accesses from the same Observation.  Sorting makes
  // each base's concrete contiguous accesses one interval-sweep run.
  for (Observation &Obs : G) {
    Obs.sort(compareForCoverage);

    // Other region forms need their own coverage index; this sweep is exact
    // only for concrete contiguous half-open intervals.
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
      // Equal atomicity is directly comparable. Cross-atomic coverage requires
      // both accesses to be mergeable, hence the second witness table.
      for (auto It = Begin; It != End; ++It) {
        std::optional<ConcreteRange> Range = getConcreteRange(*It);
        assert(Range && "concrete coverage run contains a non-concrete access");
        ConcreteAccess Candidate{&*It, Range->Begin, Range->End};
        MemoryAccess &Access = *It;
        const unsigned Atomicity = Access.isAtomic();
        bool Covered = hasCoveringWitness(AllWitnesses[Atomicity], Candidate);
        if (!Covered && Access.isMergeable())
          Covered =
              hasCoveringWitness(MergeableWitnesses[!Atomicity], Candidate);

        if (Covered) {
          Redundant.push_back(&Access);
          continue;
        }

        // MemoryAccess::covers requires both accesses to be deletable, so this
        // candidate cannot become a covering witness.
        if (!Access.isDeletable())
          continue;

        updateWitness(AllWitnesses[Atomicity], Candidate);
        if (Access.isMergeable())
          updateWitness(MergeableWitnesses[Atomicity], Candidate);
      }
      Begin = End;
    }
  }

  // Keep every sweep iterator valid until all intra-observation decisions have
  // been made, then shrink the graph before the inter-observation analysis.
  return eraseAccesses(Redundant);
}

unsigned doInterROE(MAIRGraph &G, const RedundantObservationInfo &Info) {
  DenseSet<const MemoryAccess *> ExcludedSources;
  SmallVector<MemoryAccess *, 16> Redundant;

  // Query one post-intra data-flow snapshot while excluding accesses already
  // selected by this phase from serving as later witnesses.
  for (Observation &Obs : G) {
    const AvailableAccessSet &In = Info.getIn(Obs);
    for (MemoryAccess &Access : Obs) {
      if (!In.covers(Access, ExcludedSources))
        continue;
      ExcludedSources.insert(&Access);
      Redundant.push_back(&Access);
    }
  }

  // The analysis retains source pointers, so physical deletion remains after
  // all inter-observation coverage decisions.
  return eraseAccesses(Redundant);
}

} // namespace

PreservedAnalyses
RedundantObservationEliminationPass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputAccesses += MAIRStat::compute(G).getNumMemoryAccesses();
#endif
  const unsigned NumEliminatedIntra = doIntraROE(G);

  const RedundantObservationInfo &Info =
      AM.getResult<RedundantObservationAnalysis>(G);
  const unsigned NumEliminatedInter = doInterROE(G, Info);

  const unsigned NumEliminated = NumEliminatedIntra + NumEliminatedInter;
  NumEliminatedAccesses += NumEliminated;
  NumEliminatedIntraAccesses += NumEliminatedIntra;
  NumEliminatedInterAccesses += NumEliminatedInter;
  return NumEliminated == 0 ? PreservedAnalyses::all()
                            : PreservedAnalyses::none();
}

} // namespace mair
