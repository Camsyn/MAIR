#include "MAIR/Transforms/CommonObservationMotionPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRStatLog.h"
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

bool isObservationAvailableAt(const Observation &Obs,
                              const ProgramPoint &Target,
                              const DominatorTree &DT) {
  const Instruction &Anchor = Target.getAnchor();
  const AbstractAccess *Location = Obs.getContiguousRegion();
  if (!Location || Location->hasSizeExpr())
    return false;
  // An unknown offset or size names no value, so it constrains nothing here.
  auto IsAvailable = [&](const AbstractValue &V) {
    return !V.isSymbol() || isValueAvailableAt(*V.getSymbol(), Anchor, DT);
  };
  return isValueAvailableAt(*Location->getBase(), Anchor, DT) &&
         IsAvailable(Location->getOffset()) && IsAvailable(Location->getSize());
}

enum class MotionDirection { Hoist, Sink };

bool isSink(MotionDirection Direction) {
  return Direction == MotionDirection::Sink;
}

using NeighborBlocks = SmallSetVector<const BasicBlock *, 4>;
using NeighborProgramPoints = SmallVector<ProgramPoint *, 4>;

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
                            ArrayRef<const ProgramPointEdge *> CycleEdges,
                            SmallVectorImpl<Observation *> &Matches) {
  assert(!NeighborPPs.empty() &&
         "common motion requires neighbor program points");
  for (Observation &Candidate : *NeighborPPs.front()) {
    if (!Candidate.isMovable() || !Candidate.isContiguous())
      continue;
    // The copies are equivalent, so one of them answers for all.
    if (any_of(CycleEdges, [&](const ProgramPointEdge *Edge) {
          return !Edge->isOpenTo(Candidate);
        }))
      continue;

    Matches.assign({&Candidate});
    for (ProgramPoint *Neighbor : drop_begin(NeighborPPs)) {
      Observation *Match =
          findEquivalentMovableObservation(*Neighbor, Candidate);
      if (!Match)
        break;
      Matches.push_back(Match);
    }
    if (Matches.size() != NeighborPPs.size())
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
  ArrayRef<Observation *> ToMerge = Matches;
  Observation *Accumulator =
      findEquivalentMovableObservation(Target, *Matches.front());
  if (!Accumulator) {
    Accumulator = &Target.addObservation(*Matches.front());
    ToMerge = Matches.drop_front();
  }
  for (Observation *Obs : ToMerge) {
    bool Merged = Accumulator->mergeWith(*Obs);
    assert(Merged && "exact common observations must be mergeable");
    (void)Merged;
  }
  for (Observation *Obs : Matches)
    Obs->getParent()->erase(*Obs);

  if (isSink(Direction)) {
    ++NumSunkObservations;
    MAIRStatScope::add(MAIRStatField::Sunk);
  } else {
    ++NumHoistedObservations;
    MAIRStatScope::add(MAIRStatField::Hoisted);
  }
}

bool moveOneCommonObservation(MAIRGraph &G, const BasicBlock &BB,
                              MotionDirection Direction) {
  // 1. Collect the complete, de-duplicated CFG neighbor set.
  NeighborBlocks NeighborBBs;
  if (!collectNeighborBlocks(BB, Direction, NeighborBBs))
    return false;
  if (!all_of(NeighborBBs, [&](const BasicBlock *Neighbor) {
        return G.containsBlock(*Neighbor);
      }))
    return false;

  PPEdgeKind Kind = isSink(Direction) ? PPEdgeKind::PostDom : PPEdgeKind::Dom;

  // 2. Resolve exactly one boundary representative per neighbor block.
  NeighborProgramPoints NeighborPPs(
      map_range(NeighborBBs, [&](const BasicBlock *NeighborBB) {
        return isSink(Direction) ? &G.getBBExitRepresentative(*NeighborBB)
                                 : &G.getBBEntryRepresentative(*NeighborBB);
      }));

  // 3. Require every neighbor boundary to share the typed parent Target.
  ProgramPoint &Target = isSink(Direction) ? G.getBBEntryRepresentative(BB)
                                           : G.getBBExitRepresentative(BB);
  if (!Target.hasAlwaysGuard())
    return false;
  // Sinking around a backedge would evaluate the address with the values of
  // the next iteration, so an edge that iterates a cycle takes only
  // observations that are invariant in it.
  SmallVector<const ProgramPointEdge *, 4> CycleEdges;
  for (ProgramPoint *Neighbor : NeighborPPs) {
    if (Neighbor == &Target || !Neighbor->hasAlwaysGuard())
      return false;
    const ProgramPointEdge *Edge = Neighbor->findParent(Target, Kind);
    if (!Edge)
      return false;
    if (Edge->getIteratedCycle())
      CycleEdges.push_back(Edge);
  }

  // 4. Find one observation common to every neighbor program point.
  SmallVector<Observation *, 4> Matches;
  if (!findCommonObservations(NeighborPPs, Target, CycleEdges, Matches))
    return false;

  // 5. Move the common observations to Target. The fixed-point loop restarts
  // the scan.
  moveObservationsToTarget(Target, Matches, Direction);
  return true;
}

bool moveCommonObservationsToFixedPoint(MAIRGraph &G,
                                        MotionDirection Direction) {
  const Function *F = G.getFunction();
  assert(F && "CommonObservationMotion requires a function-backed graph");
  // Each move restarts the scan from the first block.
  bool Changed = false;
  while (any_of(*F, [&](const BasicBlock &BB) {
    return G.containsBlock(BB) && moveOneCommonObservation(G, BB, Direction);
  }))
    Changed = true;
  return Changed;
}

} // namespace

PreservedAnalyses CommonObservationMotionPass::run(MAIRGraph &G,
                                                   MAIRGraphAnalysisManager &) {
  MAIRStatScope Scope("common", Ordinal, G, NumInputObservations, [&] {
    return MAIRStat::compute(G).getNumObservations();
  });
  bool Changed = moveCommonObservationsToFixedPoint(G, MotionDirection::Hoist);
  Changed |= moveCommonObservationsToFixedPoint(G, MotionDirection::Sink);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
