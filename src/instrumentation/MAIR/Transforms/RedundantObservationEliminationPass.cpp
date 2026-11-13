#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/MAIRStatLog.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CheckedArithmetic.h"

#include <array>
#include <iterator>
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
  uint64_t End = 0;
};

/// Per coverage level, the witness with the furthest end, if any.
using WitnessesByLevel = std::array<ConcreteObservation, 256>;
using WitnessesByAtomicity = std::array<WitnessesByLevel, 2>;

std::optional<ConcreteRange> getConcreteRange(const Observation &Obs) {
  if (!Obs.hasConcreteRange())
    return std::nullopt;
  const AbstractAccess &Region = *Obs.getContiguousRegion();
  uint64_t Begin = Region.getOffset().getConstant();
  std::optional<uint64_t> End =
      checkedAddUnsigned(Begin, Region.getSize().getConstant());
  if (!End)
    return std::nullopt;
  return ConcreteRange{Begin, *End};
}

bool compareForCoverage(const Observation &LHS, const Observation &RHS) {
  const MemoryObject &LObject = LHS.getMemoryObject();
  const MemoryObject &RObject = RHS.getMemoryObject();
  if (&LObject != &RObject)
    return LObject.getID() < RObject.getID();

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
    const ConcreteObservation &Witness = Witnesses[Level];
    if (Witness.Obs && Witness.End >= Target.End &&
        Witness.Obs->covers(*Target.Obs))
      return true;
  }
  return false;
}

void updateWitness(WitnessesByLevel &Witnesses,
                   const ConcreteObservation &Candidate) {
  ConcreteObservation &Witness = Witnesses[Candidate.Obs->getLevel()];
  if (!Witness.Obs || Candidate.End > Witness.End)
    Witness = Candidate;
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
        Observation &Obs = *It;
        ConcreteObservation Candidate{&Obs, Range->End};
        const unsigned Atomicity = Obs.isAtomic();
        bool Covered = hasCoveringWitness(AllWitnesses[Atomicity], Candidate);
        if (!Covered && Obs.isMergeable())
          Covered =
              hasCoveringWitness(MergeableWitnesses[!Atomicity], Candidate);

        if (Covered) {
          Redundant.push_back(&Obs);
          continue;
        }

        // A pinned observation can witness later deletions, but it is not a
        // candidate for deletion itself.
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
    for (Observation &Obs : PP) {
      if (!Info.isCovered(Obs, ExcludedSources))
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
  MAIRStatScope Scope("roe", Ordinal, G, NumInputObservations, [&] {
    return MAIRStat::compute(G).getNumObservations();
  });
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
  MAIRStatScope::add(MAIRStatField::Intra, NumEliminatedIntra);
  MAIRStatScope::add(MAIRStatField::Inter, NumEliminatedInter);
  MAIRStatScope::add(MAIRStatField::Eliminated, NumEliminated);
  return NumEliminated == 0 ? PreservedAnalyses::all()
                            : PreservedAnalyses::none();
}

} // namespace mair
