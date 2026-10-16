#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/Transforms/TransformUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

using namespace llvm;

#define DEBUG_TYPE "mair-loop-invariant-motion"

STATISTIC(NumInputObservations,
          "Number of observations entering loop-invariant motion");
STATISTIC(NumMovedToPreheaders,
          "Number of invariant observations moved to loop preheaders");
STATISTIC(NumMovedToExits,
          "Number of invariant observations moved to loop exits");

namespace mair {

namespace {

bool isAbstractValueAvailableAt(const AbstractValue &Value,
                                const ProgramPointSite &Site,
                                const MAIRLoopInfo &Loops) {
  if (Value.isConstant())
    return true;
  return Value.isSymbol() && Loops.isValueAvailableAt(*Value.getSymbol(), Site);
}

bool isRegionAvailableAt(const AbstractAccess &Region,
                         const ProgramPointSite &Site,
                         const MAIRLoopInfo &Loops) {
  return Region.getBase() &&
         Loops.isValueAvailableAt(*Region.getBase(), Site) &&
         isAbstractValueAvailableAt(Region.getOffset(), Site, Loops) &&
         isAbstractValueAvailableAt(Region.getSize(), Site, Loops);
}

std::optional<AnyProvenanceExecutedGuard>
makeExecutionGuard(const Observation &Obs) {
  SmallVector<Instruction *, 4> Provenance;
  for (Instruction *I : Obs.getInstructions()) {
    if (!I)
      return std::nullopt;
    if (!llvm::is_contained(Provenance, I))
      Provenance.push_back(I);
  }
  if (Provenance.empty())
    return std::nullopt;
  return AnyProvenanceExecutedGuard(Provenance);
}

ProgramPoint *findProgramPoint(MAIRGraph &G, const ProgramPointSite &Site,
                               const ProgramPointGuard &Guard) {
  for (ProgramPoint &PP : G)
    if (PP.getSite() == Site && PP.getGuard() == Guard)
      return &PP;
  return nullptr;
}

void moveObservationTo(Observation &Obs, ProgramPoint &Target,
                       bool MergeEquivalentObservations) {
  Observation *Accumulator = nullptr;
  if (MergeEquivalentObservations) {
    for (Observation &Existing : Target)
      if (isEquivalentMovableObservation(Existing, Obs)) {
        Accumulator = &Existing;
        break;
      }
  }

  if (!Accumulator) {
    Target.addObservation(Obs);
  } else {
    bool Merged = Accumulator->mergeWith(Obs);
    assert(Merged && "exactly matching observations must be mergeable");
    (void)Merged;
  }
  Obs.getParent()->erase(Obs);
}

enum class InvariantMotionKind { Preheader, Exit };

struct InvariantMotionPlan {
  Observation *Obs = nullptr;
  ProgramPoint *Target = nullptr;
  std::optional<ProgramPointSite> NewTargetSite;
  ProgramPointGuard Guard = AlwaysGuard{};
  InvariantMotionKind Kind = InvariantMotionKind::Preheader;
};

std::optional<InvariantMotionPlan>
findInvariantMotionForLoop(MAIRGraph &G, MAIRLoopInfo &Loops,
                           ProgramPoint &Source, Observation &Obs,
                           const AbstractAccess &Region, Loop &L) {
  ProgramPoint *Preheader = Loops.getPreheaderExit(L);
  if (Preheader && Preheader != &Source &&
      G.isAncestor(*Preheader, Source, PPEdgeKind::Dom) &&
      G.isAncestor(Source, *Preheader, PPEdgeKind::PostDom) &&
      isRegionAvailableAt(Region, Preheader->getSite(), Loops))
    return InvariantMotionPlan{&Obs, Preheader, std::nullopt, AlwaysGuard{},
                               InvariantMotionKind::Preheader};

  std::optional<MAIRLoopExit> Exit = Loops.describeUniqueExit(L);
  ProgramPoint *ExitAncestor = Loops.getUniqueExitEntry(L);
  if (!Exit || !ExitAncestor ||
      !G.isAncestor(*ExitAncestor, Source, PPEdgeKind::PostDom))
    return std::nullopt;

  std::optional<AnyProvenanceExecutedGuard> ExecutionGuard =
      makeExecutionGuard(Obs);
  if (!ExecutionGuard)
    return std::nullopt;

  ProgramPointSite TargetSite =
      Exit->IsDedicated ? ExitAncestor->getSite()
                        : ProgramPointSite::edge(*Exit->Exiting, *Exit->Exit);
  if (!isRegionAvailableAt(Region, TargetSite, Loops))
    return std::nullopt;

  ProgramPointGuard Guard = std::move(*ExecutionGuard);
  ProgramPoint *Target = findProgramPoint(G, TargetSite, Guard);
  std::optional<ProgramPointSite> NewTargetSite;
  if (!Target)
    NewTargetSite = TargetSite;
  return InvariantMotionPlan{&Obs, Target, NewTargetSite, std::move(Guard),
                             InvariantMotionKind::Exit};
}

std::optional<InvariantMotionPlan> findInvariantMotion(MAIRGraph &G,
                                                       MAIRLoopInfo &Loops) {
  if (!Loops.isAvailable())
    return std::nullopt;

  for (ProgramPoint &PP : G) {
    // Edge program points are already placed at a virtual CFG edge. Their
    // source terminator anchor identifies that edge; it does not make the
    // program point an ordinary loop-body candidate for another motion round.
    if (PP.isEdgeSite() || !PP.hasAlwaysGuard())
      continue;
    Loop *L = Loops.getLoopFor(PP);
    if (!L)
      continue;

    for (Observation &Obs : PP) {
      if (!Obs.isMovable())
        continue;
      const AbstractAccess *Region = Obs.getContiguousRegion();
      if (!Region)
        continue;

      Loop *Top = Loops.getTopInvariantLoop(Obs, *L);
      if (!Top)
        continue;

      // Invariance defines the outer bound, not whether motion to that loop is
      // legal. Try candidates from outermost to innermost so graph barriers or
      // unavailable values at an outer target can fall back to an inner one.
      SmallVector<Loop *, 4> InvariantLoops;
      for (Loop *Candidate = L; Candidate;
           Candidate = Candidate->getParentLoop()) {
        InvariantLoops.push_back(Candidate);
        if (Candidate == Top)
          break;
      }
      assert(InvariantLoops.back() == Top &&
             "top invariant loop must be an ancestor of the source loop");
      for (Loop *Candidate : llvm::reverse(InvariantLoops))
        if (std::optional<InvariantMotionPlan> Plan =
                findInvariantMotionForLoop(G, Loops, PP, Obs, *Region,
                                           *Candidate))
          return Plan;
    }
  }
  return std::nullopt;
}

void commitInvariantMotion(MAIRGraph &G, InvariantMotionPlan Plan,
                           bool MergeEquivalentObservations) {
  assert(Plan.Obs && "invariant motion requires an observation");
  ProgramPoint *Target = Plan.Target;
  if (!Target) {
    assert(Plan.NewTargetSite && "new motion target requires a site");
    Target = &G.createProgramPoint(*Plan.NewTargetSite, Plan.Guard);
  }
  moveObservationTo(*Plan.Obs, *Target, MergeEquivalentObservations);
  if (Plan.Kind == InvariantMotionKind::Preheader)
    ++NumMovedToPreheaders;
  else
    ++NumMovedToExits;
}

bool moveOneInvariantObservation(MAIRGraph &G, MAIRLoopInfo &Loops,
                                 bool MergeEquivalentObservations) {
  std::optional<InvariantMotionPlan> Plan = findInvariantMotion(G, Loops);
  if (!Plan)
    return false;
  commitInvariantMotion(G, std::move(*Plan), MergeEquivalentObservations);
  return true;
}

} // namespace

PreservedAnalyses
LoopInvariantObservationMotionPass::run(MAIRGraph &G,
                                        MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputObservations += MAIRStat::compute(G).getNumObservations();
#endif
  bool Changed = false;
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  while (moveOneInvariantObservation(G, Loops, MergeEquivalentObservations))
    Changed = true;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
