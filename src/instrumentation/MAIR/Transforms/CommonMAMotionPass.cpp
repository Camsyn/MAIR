#include "MAIR/Transforms/CommonMAMotionPass.h"
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

#define DEBUG_TYPE "mair-commonmamotion"

STATISTIC(NumHoistedAccesses, "Number of accesses hoisted by common MA motion");
STATISTIC(NumInputAccesses,
          "Number of memory accesses entering common MA motion");
STATISTIC(NumSunkAccesses, "Number of accesses sunk by common MA motion");

namespace mair {

namespace {

bool isValueAvailableAt(const Value *Value, const Instruction &Anchor,
                        const DominatorTree &DT) {
  const auto *Def = dyn_cast_or_null<Instruction>(Value);
  if (!Def)
    return true;
  return Def != &Anchor && DT.dominates(Def, &Anchor);
}

bool isAccessAvailableAt(const MemoryAccess &Access, const Observation &Target,
                         const DominatorTree &DT) {
  const Instruction &Anchor = Target.getAnchor();
  const AbstractAccess *Location = Access.getContiguousRegion();
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
using NeighborObservations = SmallVector<Observation *, 4>;

MemoryAccess *findMatchingAccess(Observation &Obs, const MemoryAccess &Needle) {
  for (MemoryAccess &Access : Obs)
    if (isEquivalentMovableAccess(Access, Needle))
      return &Access;
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

bool findCommonAccesses(ArrayRef<Observation *> NeighborObs,
                        Observation &Target,
                        SmallVectorImpl<MemoryAccess *> &Matches) {
  assert(!NeighborObs.empty() &&
         "common motion requires neighbor observations");
  for (MemoryAccess &Candidate : *NeighborObs.front()) {
    if (!Candidate.isMovable() || !Candidate.isContiguous())
      continue;

    Matches.clear();
    Matches.push_back(&Candidate);
    bool Common = true;
    for (Observation *Neighbor : drop_begin(NeighborObs)) {
      MemoryAccess *Match = findMatchingAccess(*Neighbor, Candidate);
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
    assert(isAccessAvailableAt(Candidate, Target,
                               Target.getParent()->getDominatorTree()) &&
           "common access operands must be available at target");
    return true;
  }
  return false;
}

void moveAccessesToTarget(Observation &Target, ArrayRef<MemoryAccess *> Matches,
                          MotionDirection Direction) {
  assert(!Matches.empty() && "common motion requires matching accesses");
  MemoryAccess *Accumulator = findMatchingAccess(Target, *Matches.front());

  size_t MergeBegin = 0;
  if (!Accumulator) {
    Accumulator = &Target.addAccess(*Matches.front());
    MergeBegin = 1;
  }
  for (size_t I = MergeBegin; I < Matches.size(); ++I) {
    bool Merged = Accumulator->mergeWith(*Matches[I]);
    assert(Merged && "exact common accesses must be mergeable");
    (void)Merged;
  }
  for (MemoryAccess *Access : Matches)
    Access->getParent()->erase(*Access);

  if (isSink(Direction))
    ++NumSunkAccesses;
  else
    ++NumHoistedAccesses;
}

bool moveOneCommonAccess(MAIRGraph &G, const BasicBlock &BB,
                         MotionDirection Direction) {
  // 1. Collect the complete, de-duplicated CFG neighbor set.
  NeighborBlocks NeighborBBs;
  if (!collectNeighborBlocks(BB, Direction, NeighborBBs))
    return false;

  ObsEdgeKind Kind =
      isSink(Direction) ? ObsEdgeKind::PostDom : ObsEdgeKind::Dom;

  // 2. Resolve exactly one boundary representative per neighbor block.
  const bool SourceIsEntry = Kind == ObsEdgeKind::Dom;
  NeighborObservations NeighborObs(
      map_range(NeighborBBs, [&](const BasicBlock *NeighborBB) {
        return SourceIsEntry ? &G.getBBEntryRepresentative(*NeighborBB)
                             : &G.getBBExitRepresentative(*NeighborBB);
      }));

  // 3. Require every neighbor boundary to share the typed parent Target.
  Observation &Target = isSink(Direction) ? G.getBBEntryRepresentative(BB)
                                          : G.getBBExitRepresentative(BB);
  if (!all_of(NeighborObs, [&](Observation *Neighbor) {
        return Neighbor != &Target && Neighbor->hasParent(Target, Kind);
      }))
    return false;

  // 4. Find one MA common to every neighbor observation.
  SmallVector<MemoryAccess *, 4> Matches;
  if (!findCommonAccesses(NeighborObs, Target, Matches))
    return false;

  // 5. Move the common MAs to Target. The fixed-point loop restarts the scan.
  moveAccessesToTarget(Target, Matches, Direction);
  return true;
}

bool moveCommonAccessesToFixedPoint(MAIRGraph &G, MotionDirection Direction) {
  bool Changed = false;
  const Function *F = G.getFunction();
  assert(F && "CommonMAMotion requires a function-backed graph");
  while (true) {
    bool Moved = false;
    for (const BasicBlock &BB : *F) {
      if (!moveOneCommonAccess(G, BB, Direction))
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

PreservedAnalyses CommonMAMotionPass::run(MAIRGraph &G,
                                          MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputAccesses += MAIRStat::compute(G).getNumMemoryAccesses();
#else
  (void)AM;
#endif
  bool Changed = moveCommonAccessesToFixedPoint(G, MotionDirection::Hoist);
  Changed |= moveCommonAccessesToFixedPoint(G, MotionDirection::Sink);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
