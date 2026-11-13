#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRStatLog.h"
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

bool isRegionAvailableAt(const AbstractAccess &Region,
                         const ProgramPointSite &Site,
                         const MAIRLoopInfo &Loops) {
  return Region.getBase() &&
         Loops.isValueAvailableAt(*Region.getBase(), Site) &&
         Loops.isValueAvailableAt(Region.getOffset(), Site) &&
         Loops.isValueAvailableAt(Region.getSize(), Site);
}

std::optional<AnyProvenanceExecutedGuard>
makeExecutionGuard(const Observation &Obs, BasicBlock *ResetBlock) {
  SmallVector<Instruction *, 4> Provenance;
  for (Instruction *I : Obs.getInstructions()) {
    if (!I)
      return std::nullopt;
    if (!llvm::is_contained(Provenance, I))
      Provenance.push_back(I);
  }
  if (Provenance.empty())
    return std::nullopt;
  return AnyProvenanceExecutedGuard(Provenance, ResetBlock);
}

void moveObservationTo(Observation &Obs, ProgramPoint &Target,
                       bool MergeEquivalentObservations) {
  Observation *Accumulator = MergeEquivalentObservations
                                 ? findEquivalentMovableObservation(Target, Obs)
                                 : nullptr;
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

std::optional<InvariantMotionPlan> findInvariantMotion(MAIRLoopInfo &Loops,
                                                       ProgramPoint &Source,
                                                       Observation &Obs,
                                                       const Loop &L) {
  if (!Obs.isMovable())
    return std::nullopt;
  const AbstractAccess *Region = Obs.getContiguousRegion();
  if (!Region || !Loops.isLoopInvariant(Obs, L))
    return std::nullopt;

  ProgramPoint *Preheader = Loops.getPreheaderExit(L);
  if (Preheader && Preheader != &Source &&
      Loops.canHoistToPreheader(Source, L) &&
      isRegionAvailableAt(*Region, Preheader->getSite(), Loops))
    return InvariantMotionPlan{&Obs, Preheader, std::nullopt, AlwaysGuard{},
                               InvariantMotionKind::Preheader};

  std::optional<ProgramPointSite> TargetSite = Loops.getExitSite(L);
  if (!TargetSite || !Loops.canSinkToExit(Source, L))
    return std::nullopt;

  std::optional<AnyProvenanceExecutedGuard> ExecutionGuard =
      makeExecutionGuard(Obs, L.getLoopPreheader());
  if (!ExecutionGuard)
    return std::nullopt;

  if (!isRegionAvailableAt(*Region, *TargetSite, Loops))
    return std::nullopt;

  ProgramPointGuard Guard = std::move(*ExecutionGuard);
  ProgramPoint *Target = Loops.findExitPoint(L, *TargetSite, Guard);
  std::optional<ProgramPointSite> NewTargetSite;
  if (!Target)
    NewTargetSite = TargetSite;
  return InvariantMotionPlan{&Obs, Target, NewTargetSite, std::move(Guard),
                             InvariantMotionKind::Exit};
}

void commitInvariantMotion(MAIRGraph &G, InvariantMotionPlan Plan,
                           bool MergeEquivalentObservations) {
  assert(Plan.Obs && "invariant motion requires an observation");
  ProgramPoint *Target = Plan.Target;
  if (!Target) {
    assert(Plan.NewTargetSite && "new motion target requires a site");
    Target =
        &G.getParent()->createProgramPoint(*Plan.NewTargetSite, Plan.Guard);
  }
  moveObservationTo(*Plan.Obs, *Target, MergeEquivalentObservations);
  if (Plan.Kind == InvariantMotionKind::Preheader) {
    ++NumMovedToPreheaders;
    MAIRStatScope::add(MAIRStatField::Preheader);
  } else {
    ++NumMovedToExits;
    MAIRStatScope::add(MAIRStatField::Exit);
  }
}

} // namespace

PreservedAnalyses
LoopInvariantObservationMotionPass::run(MAIRGraph &G,
                                        MAIRGraphAnalysisManager &AM) {
  MAIRStatScope Scope("liom", Ordinal, G, NumInputObservations, [&] {
    return MAIRStat::compute(G).getNumObservations();
  });
  bool Changed = false;
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  const Loop *ScopedLoop = G.getLoop();
  if (!Loops.isAvailable() || !ScopedLoop)
    return PreservedAnalyses::all();
  for (ProgramPoint &PP : G) {
    // Edge program points are already placed at a virtual CFG edge. Their
    // source terminator anchor identifies that edge; it does not make the
    // program point an ordinary loop-body candidate for another motion round.
    if (PP.isEdgeSite() || !PP.hasAlwaysGuard())
      continue;
    for (Observation &Obs : llvm::make_early_inc_range(PP)) {
      std::optional<InvariantMotionPlan> Plan =
          findInvariantMotion(Loops, PP, Obs, *ScopedLoop);
      if (!Plan)
        continue;
      commitInvariantMotion(G, std::move(*Plan), MergeEquivalentObservations);
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
