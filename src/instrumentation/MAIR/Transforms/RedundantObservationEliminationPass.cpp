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

STATISTIC(NumEliminatedObservations,
          "Number of redundant observations eliminated");
STATISTIC(NumEliminatedIntraObservations,
          "Number of intra-program-point redundant observations eliminated");
STATISTIC(NumEliminatedInterObservations,
          "Number of inter-program-point redundant observations eliminated");
STATISTIC(NumInputObservations,
          "Number of observations entering redundant observation elimination");

namespace mair {

namespace {

struct ConcreteRange {
  uint64_t Begin = 0;
  uint64_t End = 0;
};

struct ConcreteObservation {
  Observation *Obs = nullptr;
  uint64_t Begin = 0;
  uint64_t End = 0;
};

struct CoveringWitness {
  Observation *Obs = nullptr;
  uint64_t End = 0;
};

using WitnessesByLevel = std::array<CoveringWitness, 256>;
using WitnessesByAtomicity = std::array<WitnessesByLevel, 2>;

std::optional<ConcreteRange> getConcreteRange(const Observation &Obs) {
  const AbstractAccess *Region = Obs.getContiguousRegion();
  if (!Region || !Region->getOffset().isConstant() ||
      !Region->getSize().isConstant())
    return std::nullopt;

  uint64_t Begin = Region->getOffset().getConstant();
  uint64_t Size = Region->getSize().getConstant();
  if (Size > std::numeric_limits<uint64_t>::max() - Begin)
    return std::nullopt;
  return ConcreteRange{Begin, Begin + Size};
}

bool compareForCoverage(const Observation &LHS, const Observation &RHS) {
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
                        const ConcreteObservation &Target) {
  // The sweep guarantees that every witness starts no later than Target.  The
  // furthest end at each covering level is therefore the only candidate needed.
  for (unsigned Level = Target.Obs->getLevel(); Level < Witnesses.size();
       ++Level) {
    const CoveringWitness &Witness = Witnesses[Level];
    if (Witness.Obs && Witness.End >= Target.End &&
        Witness.Obs->covers(*Target.Obs))
      return true;
  }
  return false;
}

void updateWitness(WitnessesByLevel &Witnesses,
                   const ConcreteObservation &Candidate) {
  CoveringWitness &Witness = Witnesses[Candidate.Obs->getLevel()];
  if (!Witness.Obs || Candidate.End > Witness.End)
    Witness = {Candidate.Obs, Candidate.End};
}

unsigned eraseObservations(SmallVectorImpl<Observation *> &Observations) {
  for (Observation *Obs : Observations)
    Obs->eraseFromParent();
  return Observations.size();
}

unsigned doIntraROE(MAIRGraph &G) {
  SmallVector<Observation *, 16> Redundant;

  // Phase 1 considers only observations from the same ProgramPoint.  Sorting
  // makes each base's concrete contiguous observations one interval-sweep run.
  for (ProgramPoint &PP : G) {
    PP.sort(compareForCoverage);

    // Other region forms need their own coverage index; this sweep is exact
    // only for concrete contiguous half-open intervals.
    for (auto Begin = PP.begin(); Begin != PP.end();) {
      std::optional<ConcreteRange> FirstRange = getConcreteRange(*Begin);
      if (!FirstRange) {
        ++Begin;
        continue;
      }

      auto End = std::next(Begin);
      while (End != PP.end() && End->getBase() == Begin->getBase() &&
             getConcreteRange(*End))
        ++End;

      WitnessesByAtomicity AllWitnesses{};
      WitnessesByAtomicity MergeableWitnesses{};
      // Equal atomicity is directly comparable. Cross-atomic coverage requires
      // both observations to be mergeable, hence the second witness table.
      for (auto It = Begin; It != End; ++It) {
        std::optional<ConcreteRange> Range = getConcreteRange(*It);
        assert(Range &&
               "concrete coverage run contains a non-concrete observation");
        ConcreteObservation Candidate{&*It, Range->Begin, Range->End};
        Observation &Obs = *It;
        const unsigned Atomicity = Obs.isAtomic();
        bool Covered = hasCoveringWitness(AllWitnesses[Atomicity], Candidate);
        if (!Covered && Obs.isMergeable())
          Covered =
              hasCoveringWitness(MergeableWitnesses[!Atomicity], Candidate);

        if (Covered) {
          Redundant.push_back(&Obs);
          continue;
        }

        // Observation::covers requires both observations to be deletable, so
        // this candidate cannot become a covering witness.
        if (!Obs.isDeletable())
          continue;

        updateWitness(AllWitnesses[Atomicity], Candidate);
        if (Obs.isMergeable())
          updateWitness(MergeableWitnesses[Atomicity], Candidate);
      }
      Begin = End;
    }
  }

  // Keep every sweep iterator valid until all intra-program-point decisions
  // have been made, then shrink the graph before the inter-program-point
  // analysis.
  return eraseObservations(Redundant);
}

unsigned doInterROE(MAIRGraph &G, const RedundantObservationInfo &Info) {
  DenseSet<const Observation *> ExcludedSources;
  SmallVector<Observation *, 16> Redundant;

  // Query one post-intra data-flow snapshot while excluding observations
  // already selected by this phase from serving as later witnesses.
  for (ProgramPoint &PP : G) {
    const AvailableObservationSet &In = Info.getIn(PP);
    for (Observation &Obs : PP) {
      if (!In.covers(Obs, ExcludedSources))
        continue;
      ExcludedSources.insert(&Obs);
      Redundant.push_back(&Obs);
    }
  }

  // The analysis retains source pointers, so physical deletion remains after
  // all inter-program-point coverage decisions.
  return eraseObservations(Redundant);
}

} // namespace

PreservedAnalyses
RedundantObservationEliminationPass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputObservations += MAIRStat::compute(G).getNumObservations();
#endif
  const unsigned NumEliminatedIntra = doIntraROE(G);

  // Intra elimination changes the source set. Compute this pass's inter
  // snapshot from the current graph instead of reusing a possibly cached
  // pre-intra result; unrelated analyses remain untouched.
  RedundantObservationInfo Info = RedundantObservationAnalysis().run(G, AM);
  const unsigned NumEliminatedInter = doInterROE(G, Info);

  const unsigned NumEliminated = NumEliminatedIntra + NumEliminatedInter;
  NumEliminatedObservations += NumEliminated;
  NumEliminatedIntraObservations += NumEliminatedIntra;
  NumEliminatedInterObservations += NumEliminatedInter;
  return NumEliminated == 0 ? PreservedAnalyses::all()
                            : PreservedAnalyses::none();
}

} // namespace mair
