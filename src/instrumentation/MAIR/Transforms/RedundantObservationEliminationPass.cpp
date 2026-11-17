#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/MAIRStatLog.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
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

/// Select the observations of [Begin, End), the observations of one memory
/// object that have no concrete range, that another of them covers. Every
/// pair is tried. Targets are visited last to first and witnesses first to
/// last, so of two observations that cover each other the first in the list
/// stays. A witness is never deleted afterwards: coverage need not be
/// transitive (cross-atomic coverage needs both sides mergeable), so each
/// deleted observation keeps its own witness.
void eliminateNonConcreteRun(ProgramPoint::iterator Begin,
                             ProgramPoint::iterator End,
                             SmallVectorImpl<Observation *> &Redundant,
                             ScalarEvolution *SE) {
  SmallVector<Observation *, 8> Run;
  for (auto It = Begin; It != End; ++It)
    Run.push_back(&*It);
  SmallPtrSet<const Observation *, 8> Deleted, Witnesses;
  for (Observation *Target : reverse(Run)) {
    if (Witnesses.contains(Target))
      continue;
    for (Observation *Witness : Run) {
      if (Witness == Target || Deleted.contains(Witness) ||
          !Witness->covers(*Target, SE))
        continue;
      Deleted.insert(Target);
      Witnesses.insert(Witness);
      Redundant.push_back(Target);
      break;
    }
  }
}

unsigned doIntraROE(MAIRGraph &G, ScalarEvolution *SE) {
  SmallVector<Observation *, 16> Redundant;

  // Phase 1 considers only observations from the same ProgramPoint.  Sorting
  // makes each base's concrete contiguous observations one interval-sweep run,
  // followed by a run of its other observations.
  //
  // The sort is stable, so of observations with equal ranges and levels the
  // first in the list stays. In default<O1>, which the sanitizers run, motion
  // merges such observations into one and keeps all their sources. Without
  // merging, in default<Og>, invariant motion and periodic combination append
  // them in the list order of the loop's graph, which follows the block
  // layout, not necessarily the order their sources run in.
  for (ProgramPoint &PP : G) {
    PP.sort(compareForCoverage);

    // The sweep is exact only for concrete contiguous half-open intervals.
    // The other observations of an object follow its concrete ones; they
    // have no order to sweep and are compared pairwise.
    for (auto Begin = PP.begin(); Begin != PP.end();) {
      std::optional<ConcreteRange> FirstRange = getConcreteRange(*Begin);
      if (!FirstRange) {
        auto End = std::next(Begin);
        while (End != PP.end() &&
               &End->getMemoryObject() == &Begin->getMemoryObject())
          ++End;
        eliminateNonConcreteRun(Begin, End, Redundant, SE);
        Begin = End;
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

/// The program points of \p G, each after every program point it reaches
/// along Dom edges: a postorder of the Dom edges, which are acyclic.
SmallVector<ProgramPoint *, 32> dominatedFirst(MAIRGraph &G) {
  SmallVector<ProgramPoint *, 32> Order;
  Order.reserve(G.size());
  SmallPtrSet<const ProgramPoint *, 32> Visited;
  // A program point and the index of the next child edge to look at.
  SmallVector<std::pair<ProgramPoint *, unsigned>, 16> Stack;
  for (ProgramPoint &Root : G) {
    if (!Visited.insert(&Root).second)
      continue;
    Stack.push_back({&Root, 0});
    while (!Stack.empty()) {
      auto &[PP, Next] = Stack.back();
      ArrayRef<ProgramPointEdge> Children = PP->children();
      if (Next == Children.size()) {
        Order.push_back(PP);
        Stack.pop_back();
        continue;
      }
      const ProgramPointEdge &Edge = Children[Next++];
      if (Edge.isDom() && Visited.insert(&Edge.getNode()).second)
        Stack.push_back({&Edge.getNode(), 0});
    }
  }
  return Order;
}

unsigned doInterROE(MAIRGraph &G, const RedundantObservationInfo &Info,
                    ScalarEvolution *SE) {
  DenseSet<const Observation *> ExcludedSources;
  SmallVector<Observation *, 16> Redundant;

  // Query one post-intra data-flow snapshot while excluding observations
  // already selected by this phase from serving as later witnesses.
  //
  // The order decides which of two observations that cover each other
  // stays: the one examined second, because the first is removed and no
  // longer witnesses. Of two observations with equal levels where A dominates B
  // and B post-dominates A, A stays, as it runs first:
  //
  //   entry: %a = load i32, ptr %p   ; stays
  //          br i1 %c, label %then, label %join
  //   then:  br label %join
  //   join:  %b = load i32, ptr %p   ; removed
  //
  // So every program point is examined before the program points that
  // reach it along Dom edges. The list order of the graph follows the block
  // layout, which need not be a dominance order. Program points that no Dom
  // path connects keep the order of the traversal; any order is sound, it
  // only picks the survivor. Observations of one program point never witness
  // each other here, so their order does not matter.
  for (ProgramPoint *PP : dominatedFirst(G)) {
    for (Observation &Obs : *PP) {
      if (!Info.isCovered(Obs, ExcludedSources, SE))
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
  // ScalarEvolution proves constant distances between symbolic addresses
  // (AbstractAccess::contains). The pass does not change the IR, so the
  // analysis stays valid throughout.
  ScalarEvolution *SE = nullptr;
  if (AM.isPassRegistered<LoopAnalysis>()) {
    MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
    if (Loops.isAvailable())
      SE = &Loops.getSE();
  }
  const unsigned NumEliminatedIntra = doIntraROE(G, SE);

  // Intra elimination changes the source set. Compute this pass's inter
  // snapshot from the current graph instead of reusing a possibly cached
  // pre-intra result; unrelated analyses remain untouched.
  RedundantObservationInfo Info = RedundantObservationAnalysis().run(G, AM);
  const unsigned NumEliminatedInter = doInterROE(G, Info, SE);

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
