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

STATISTIC(NumInputAccesses,
          "Number of memory accesses entering loop-invariant motion");
STATISTIC(NumMovedToPreheaders,
          "Number of invariant accesses moved to loop preheaders");
STATISTIC(NumMovedToExits, "Number of invariant accesses moved to loop exits");

namespace mair {

namespace {

bool isAbstractValueAvailableAt(const AbstractValue &Value,
                                const ObservationSite &Site,
                                const MAIRLoopInfo &Loops) {
  if (Value.isConstant())
    return true;
  return Value.isSymbol() && Loops.isValueAvailableAt(*Value.getSymbol(), Site);
}

bool isRegionAvailableAt(const AbstractAccess &Region,
                         const ObservationSite &Site,
                         const MAIRLoopInfo &Loops) {
  return Region.getBase() &&
         Loops.isValueAvailableAt(*Region.getBase(), Site) &&
         isAbstractValueAvailableAt(Region.getOffset(), Site, Loops) &&
         isAbstractValueAvailableAt(Region.getSize(), Site, Loops);
}

std::optional<AnyProvenanceExecutedGuard>
makeExecutionGuard(const MemoryAccess &Access) {
  SmallVector<Instruction *, 4> Provenance;
  for (Instruction *I : Access.getInstructions()) {
    if (!I)
      return std::nullopt;
    if (!llvm::is_contained(Provenance, I))
      Provenance.push_back(I);
  }
  if (Provenance.empty())
    return std::nullopt;
  return AnyProvenanceExecutedGuard(Provenance);
}

Observation *findEvent(MAIRGraph &G, const ObservationSite &Site,
                       const ObservationGuard &Guard) {
  for (Observation &Obs : G)
    if (Obs.getSite() == Site && Obs.getGuard() == Guard)
      return &Obs;
  return nullptr;
}

void moveAccessTo(MemoryAccess &Access, Observation &Target,
                  bool MergeEquivalentAccesses) {
  MemoryAccess *Accumulator = nullptr;
  if (MergeEquivalentAccesses) {
    for (MemoryAccess &Existing : Target)
      if (isEquivalentMovableAccess(Existing, Access)) {
        Accumulator = &Existing;
        break;
      }
  }

  if (!Accumulator) {
    Target.addAccess(Access);
  } else {
    bool Merged = Accumulator->mergeWith(Access);
    assert(Merged && "exactly matching accesses must be mergeable");
    (void)Merged;
  }
  Access.getParent()->erase(Access);
}

enum class InvariantMotionKind { Preheader, Exit };

struct InvariantMotionPlan {
  MemoryAccess *Access = nullptr;
  Observation *Target = nullptr;
  std::optional<ObservationSite> NewTargetSite;
  ObservationGuard Guard = AlwaysGuard{};
  InvariantMotionKind Kind = InvariantMotionKind::Preheader;
};

std::optional<InvariantMotionPlan>
findInvariantMotionForLoop(MAIRGraph &G, MAIRLoopInfo &Loops,
                           Observation &Source, MemoryAccess &Access,
                           const AbstractAccess &Region, Loop &L) {
  Observation *Preheader = Loops.getPreheaderExit(L);
  if (Preheader && Preheader != &Source &&
      G.isAncestor(*Preheader, Source, ObsEdgeKind::Dom) &&
      G.isAncestor(Source, *Preheader, ObsEdgeKind::PostDom) &&
      isRegionAvailableAt(Region, Preheader->getSite(), Loops))
    return InvariantMotionPlan{&Access, Preheader, std::nullopt, AlwaysGuard{},
                               InvariantMotionKind::Preheader};

  std::optional<MAIRLoopExit> Exit = Loops.describeUniqueExit(L);
  Observation *ExitAncestor = Loops.getUniqueExitEntry(L);
  if (!Exit || !ExitAncestor ||
      !G.isAncestor(*ExitAncestor, Source, ObsEdgeKind::PostDom))
    return std::nullopt;

  std::optional<AnyProvenanceExecutedGuard> ExecutionGuard =
      makeExecutionGuard(Access);
  if (!ExecutionGuard)
    return std::nullopt;

  ObservationSite TargetSite =
      Exit->IsDedicated ? ExitAncestor->getSite()
                        : ObservationSite::edge(*Exit->Exiting, *Exit->Exit);
  if (!isRegionAvailableAt(Region, TargetSite, Loops))
    return std::nullopt;

  ObservationGuard Guard = std::move(*ExecutionGuard);
  Observation *Target = findEvent(G, TargetSite, Guard);
  std::optional<ObservationSite> NewTargetSite;
  if (!Target)
    NewTargetSite = TargetSite;
  return InvariantMotionPlan{&Access, Target, NewTargetSite, std::move(Guard),
                             InvariantMotionKind::Exit};
}

std::optional<InvariantMotionPlan> findInvariantMotion(MAIRGraph &G,
                                                       MAIRLoopInfo &Loops) {
  if (!Loops.isAvailable())
    return std::nullopt;

  for (Observation &Obs : G) {
    if (!Obs.hasAlwaysGuard())
      continue;
    Loop *L = Loops.getLoopFor(Obs);
    if (!L)
      continue;

    for (MemoryAccess &Access : Obs) {
      if (!Access.isMovable())
        continue;
      const AbstractAccess *Region = Access.getContiguousRegion();
      if (!Region)
        continue;

      Loop *Top = Loops.getTopInvariantLoop(Access, *L);
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
                findInvariantMotionForLoop(G, Loops, Obs, Access, *Region,
                                           *Candidate))
          return Plan;
    }
  }
  return std::nullopt;
}

void commitInvariantMotion(MAIRGraph &G, InvariantMotionPlan Plan,
                           bool MergeEquivalentAccesses) {
  assert(Plan.Access && "invariant motion requires an access");
  Observation *Target = Plan.Target;
  if (!Target) {
    assert(Plan.NewTargetSite && "new motion target requires a site");
    Target = &G.createObservation(*Plan.NewTargetSite, Plan.Guard);
  }
  moveAccessTo(*Plan.Access, *Target, MergeEquivalentAccesses);
  if (Plan.Kind == InvariantMotionKind::Preheader)
    ++NumMovedToPreheaders;
  else
    ++NumMovedToExits;
}

bool moveOneInvariantAccess(MAIRGraph &G, MAIRLoopInfo &Loops,
                            bool MergeEquivalentAccesses) {
  std::optional<InvariantMotionPlan> Plan = findInvariantMotion(G, Loops);
  if (!Plan)
    return false;
  commitInvariantMotion(G, std::move(*Plan), MergeEquivalentAccesses);
  return true;
}

} // namespace

PreservedAnalyses
LoopInvariantObservationMotionPass::run(MAIRGraph &G,
                                        MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputAccesses += MAIRStat::compute(G).getNumMemoryAccesses();
#endif
  bool Changed = false;
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  while (moveOneInvariantAccess(G, Loops, MergeEquivalentAccesses))
    Changed = true;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
