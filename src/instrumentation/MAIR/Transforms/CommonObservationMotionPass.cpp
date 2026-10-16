#include "MAIR/Transforms/CommonObservationMotionPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/Transforms/TransformUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"

using namespace llvm;

#define DEBUG_TYPE "mair-common-observation-motion"

STATISTIC(NumHoistedObservations,
          "Number of observations hoisted by common observation motion");
STATISTIC(NumInputObservations,
          "Number of observations entering common observation motion");
STATISTIC(NumSunkObservations,
          "Number of observations sunk by common observation motion");

namespace mair {

namespace {

bool isValueAvailableAt(const Value *Value, const Instruction &Anchor,
                        const DominatorTree &DT) {
  const auto *Def = dyn_cast_or_null<Instruction>(Value);
  if (!Def)
    return true;
  return Def != &Anchor && DT.dominates(Def, &Anchor);
}

bool isObservationAvailableAt(const Observation &Obs,
                              const ProgramPoint &Target,
                              const DominatorTree &DT) {
  const Instruction &Anchor = Target.getAnchor();
  const AbstractAccess *Location = Obs.getContiguousRegion();
  if (!Location)
    return false;
  if (!isValueAvailableAt(Location->getBase(), Anchor, DT))
    return false;
  if (Location->getOffset().isSymbol() &&
      !isValueAvailableAt(Location->getOffset().getSymbol(), Anchor, DT))
    return false;
  return !Location->getSize().isSymbol() ||
         isValueAvailableAt(Location->getSize().getSymbol(), Anchor, DT);
}

enum class MotionDirection { Hoist, Sink };

bool isSink(MotionDirection Direction) {
  return Direction == MotionDirection::Sink;
}

using NeighborBlocks = SmallSetVector<const BasicBlock *, 4>;
using NeighborProgramPoints = SmallVector<ProgramPoint *, 4>;

Observation *findMatchingObservation(ProgramPoint &PP,
                                     const Observation &Needle) {
  for (Observation &Obs : PP)
    if (isEquivalentMovableObservation(Obs, Needle))
      return &Obs;
  return nullptr;
}

bool collectNeighborBlocks(const BasicBlock &BB, MotionDirection Direction,
                           NeighborBlocks &Neighbors) {
  if (isSink(Direction))
    Neighbors.insert_range(predecessors(&BB));
  else
    Neighbors.insert_range(successors(&BB));
  return Neighbors.size() >= 2 && !Neighbors.contains(&BB);
}

bool findCommonObservations(ArrayRef<ProgramPoint *> NeighborPPs,
                            ProgramPoint &Target,
                            SmallVectorImpl<Observation *> &Matches) {
  assert(!NeighborPPs.empty() &&
         "common motion requires neighbor program points");
  for (Observation &Candidate : *NeighborPPs.front()) {
    if (!Candidate.isMovable() || !Candidate.isContiguous())
      continue;

    Matches.clear();
    Matches.push_back(&Candidate);
    bool Common = true;
    for (ProgramPoint *Neighbor : drop_begin(NeighborPPs)) {
      Observation *Match = findMatchingObservation(*Neighbor, Candidate);
      if (!Match) {
        Common = false;
        break;
      }
      Matches.push_back(Match);
    }
    if (!Common)
      continue;
    // All copies are shared by every neighbor, so their SSA operands must be
    // visible at the common target in a valid, materializable graph.
    assert(isObservationAvailableAt(Candidate, Target,
                                    Target.getParent()->getDominatorTree()) &&
           "common observation operands must be available at target");
    return true;
  }
  return false;
}

void moveObservationsToTarget(ProgramPoint &Target,
                              ArrayRef<Observation *> Matches,
                              MotionDirection Direction) {
  assert(!Matches.empty() && "common motion requires matching observations");
  Observation *Accumulator = findMatchingObservation(Target, *Matches.front());

  size_t MergeBegin = 0;
  if (!Accumulator) {
    Accumulator = &Target.addObservation(*Matches.front());
    MergeBegin = 1;
  }
  for (size_t I = MergeBegin; I < Matches.size(); ++I) {
    bool Merged = Accumulator->mergeWith(*Matches[I]);
    assert(Merged && "exact common observations must be mergeable");
    (void)Merged;
  }
  for (Observation *Obs : Matches)
    Obs->getParent()->erase(*Obs);

  if (isSink(Direction))
    ++NumSunkObservations;
  else
    ++NumHoistedObservations;
}

bool moveOneCommonObservation(MAIRGraph &G, const BasicBlock &BB,
                              MotionDirection Direction) {
  // 1. Collect the complete, de-duplicated CFG neighbor set.
  NeighborBlocks NeighborBBs;
  if (!collectNeighborBlocks(BB, Direction, NeighborBBs))
    return false;

  PPEdgeKind Kind = isSink(Direction) ? PPEdgeKind::PostDom : PPEdgeKind::Dom;

  // 2. Resolve exactly one boundary representative per neighbor block.
  const bool SourceIsEntry = Kind == PPEdgeKind::Dom;
  NeighborProgramPoints NeighborPPs(
      map_range(NeighborBBs, [&](const BasicBlock *NeighborBB) {
        return SourceIsEntry ? &G.getBBEntryRepresentative(*NeighborBB)
                             : &G.getBBExitRepresentative(*NeighborBB);
      }));

  // 3. Require every neighbor boundary to share the typed parent Target.
  ProgramPoint &Target = isSink(Direction) ? G.getBBEntryRepresentative(BB)
                                           : G.getBBExitRepresentative(BB);
  if (!all_of(NeighborPPs, [&](ProgramPoint *Neighbor) {
        return Neighbor != &Target && Neighbor->hasParent(Target, Kind);
      }))
    return false;

  // 4. Find one observation common to every neighbor program point.
  SmallVector<Observation *, 4> Matches;
  if (!findCommonObservations(NeighborPPs, Target, Matches))
    return false;

  // 5. Move the common observations to Target. The fixed-point loop restarts
  // the scan.
  moveObservationsToTarget(Target, Matches, Direction);
  return true;
}

bool moveCommonObservationsToFixedPoint(MAIRGraph &G,
                                        MotionDirection Direction) {
  bool Changed = false;
  const Function *F = G.getFunction();
  assert(F && "CommonObservationMotion requires a function-backed graph");
  while (true) {
    bool Moved = false;
    for (const BasicBlock &BB : *F) {
      if (!moveOneCommonObservation(G, BB, Direction))
        continue;
      Moved = true;
      Changed = true;
      break;
    }
    if (!Moved)
      return Changed;
  }
}

} // namespace

PreservedAnalyses
CommonObservationMotionPass::run(MAIRGraph &G, MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputObservations += MAIRStat::compute(G).getNumObservations();
#else
  (void)AM;
#endif
  bool Changed = moveCommonObservationsToFixedPoint(G, MotionDirection::Hoist);
  Changed |= moveCommonObservationsToFixedPoint(G, MotionDirection::Sink);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
