#include "MAIR/Transforms/CommonMAMotionPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/Transforms/TransformUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
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
                        DominatorTree &DT) {
  const auto *Def = dyn_cast_or_null<Instruction>(Value);
  if (!Def)
    return true;
  return Def != &Anchor && DT.dominates(Def, &Anchor);
}

bool isAccessAvailableAt(const MemoryAccess &Access, const Observation &Target,
                         DominatorTree &DT) {
  const Instruction *Anchor = Target.getAnchor();
  assert(Anchor && "motion target must have an anchor");
  const AbstractAccess *Location = Access.getContiguousRegion();
  if (!Location)
    return false;
  if (!isValueAvailableAt(Location->getBase(), *Anchor, DT))
    return false;
  if (Location->getOffset().isSymbol() &&
      !isValueAvailableAt(Location->getOffset().getSymbol(), *Anchor, DT))
    return false;
  return !Location->getSize().isSymbol() ||
         isValueAvailableAt(Location->getSize().getSymbol(), *Anchor, DT);
}

SmallPtrSet<Observation *, 16> collectReachableInBlock(Observation &Root,
                                                       ObsEdgeKind Kind,
                                                       const BasicBlock &BB) {
  SmallPtrSet<Observation *, 16> Reachable;
  SmallVector<Observation *, 16> Worklist;
  Reachable.insert(&Root);
  Worklist.push_back(&Root);
  while (!Worklist.empty()) {
    Observation *Current = Worklist.pop_back_val();
    for (const ObservationEdge &Edge : Current->children()) {
      if (Edge.getKind() != Kind)
        continue;
      Observation *Next = &Edge.getNode();
      if (!Next->representsBlock(BB))
        continue;
      if (Reachable.insert(Next).second)
        Worklist.push_back(Next);
    }
  }
  return Reachable;
}

enum class MotionDirection { Hoist, Sink };

bool isSink(MotionDirection Direction) {
  return Direction == MotionDirection::Sink;
}

bool collectCFGNeighbors(const BasicBlock &BB, MotionDirection Direction,
                         SmallVectorImpl<const BasicBlock *> &Neighbors) {
  SmallPtrSet<const BasicBlock *, 8> Seen;
  auto AddNeighbor = [&](const BasicBlock *Neighbor) {
    if (Neighbor == &BB)
      return false;
    if (Seen.insert(Neighbor).second)
      Neighbors.push_back(Neighbor);
    return true;
  };

  if (isSink(Direction)) {
    for (const BasicBlock *Pred : predecessors(&BB))
      if (!AddNeighbor(Pred))
        return false;
  } else {
    for (const BasicBlock *Succ : successors(&BB))
      if (!AddNeighbor(Succ))
        return false;
  }
  return Neighbors.size() >= 2;
}

using ObservationGroup = SmallVector<Observation *, 8>;
using SourceGroups = SmallVector<ObservationGroup, 4>;

class ObservationBlockIndex {
  DenseMap<const BasicBlock *, ObservationGroup> Groups;
  size_t GraphSize = 0;

public:
  explicit ObservationBlockIndex(MAIRGraph &G) : GraphSize(G.size()) {
    const Function *F = G.getFunction();
    assert(F && "CommonMAMotion requires a function-backed graph");
    for (const BasicBlock &BB : *F)
      for (Observation &Obs : G)
        if (Obs.representsBlock(BB))
          Groups[&BB].push_back(&Obs);
  }

  ArrayRef<Observation *> lookup(const BasicBlock &BB) const {
    auto It = Groups.find(&BB);
    if (It == Groups.end())
      return {};
    return It->second;
  }

  void assertTopologyUnchanged(const MAIRGraph &G) const {
    assert(G.size() == GraphSize &&
           "CommonMAMotion changed observation topology");
  }
};

MemoryAccess *findMatchingAccess(ArrayRef<Observation *> Group,
                                 const MemoryAccess &Needle) {
  for (Observation *Obs : Group)
    for (MemoryAccess &Access : *Obs)
      if (isEquivalentMovableAccess(Access, Needle))
        return &Access;
  return nullptr;
}

bool collectSourceGroups(MAIRGraph &G, Observation &ProofRoot,
                         ArrayRef<const BasicBlock *> Neighbors,
                         ObsEdgeKind Kind,
                         const ObservationBlockIndex &Observations,
                         SourceGroups &Groups) {
  bool SourceIsEntry = Kind == ObsEdgeKind::Dom;
  SmallPtrSet<Observation *, 8> SeenBoundaries;
  for (const BasicBlock *Neighbor : Neighbors) {
    Observation &Boundary = SourceIsEntry
                                ? G.getBBEntryRepresentative(*Neighbor)
                                : G.getBBExitRepresentative(*Neighbor);
    if (&Boundary == &ProofRoot ||
        !SeenBoundaries.insert(&Boundary).second ||
        !G.hasEdge(ProofRoot, Boundary, Kind))
      return false;

    SmallPtrSet<Observation *, 16> Reachable =
        collectReachableInBlock(Boundary, Kind, *Neighbor);
    ObservationGroup &Group = Groups.emplace_back();
    ArrayRef<Observation *> BlockObservations = Observations.lookup(*Neighbor);
    if (BlockObservations.empty())
      return false;
    for (Observation *Obs : BlockObservations)
      if (Reachable.contains(Obs))
        Group.push_back(Obs);
  }
  return true;
}

void commitCommonMotion(Observation &Target, ArrayRef<MemoryAccess *> Matches,
                        MotionDirection Direction) {
  assert(!Matches.empty() && "common motion requires matching accesses");
  MemoryAccess *Accumulator = nullptr;
  for (MemoryAccess &Access : Target)
    if (isEquivalentMovableAccess(Access, *Matches.front())) {
      Accumulator = &Access;
      break;
    }

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
                         MotionDirection Direction, DominatorTree &DT,
                         const ObservationBlockIndex &Observations) {
  // Resolve the logical entry/exit boundary after SimplifyGraph contraction,
  // then require the complete, de-duplicated CFG neighbor set. Self-edges and
  // fewer than two distinct neighbors are deliberately no-ops.
  Observation &Target = isSink(Direction) ? G.getBBEntryRepresentative(BB)
                                          : G.getBBExitRepresentative(BB);
  SmallVector<const BasicBlock *, 4> Neighbors;
  if (!collectCFGNeighbors(BB, Direction, Neighbors))
    return false;

  ObsEdgeKind Kind =
      isSink(Direction) ? ObsEdgeKind::PostDom : ObsEdgeKind::Dom;
  Observation &ProofRoot =
      isSink(Direction) ? Target : G.getBBEntryRepresentative(BB);
  if (!isSink(Direction)) {
    // Inter-block Dom proofs originate at the entry representative. When the
    // entry and exit have not contracted, the intra-block path also proves
    // that hoisting to the exit does not cross a forward barrier.
    if (&ProofRoot != &Target) {
      SmallPtrSet<Observation *, 16> TargetReachable =
          collectReachableInBlock(ProofRoot, ObsEdgeKind::Dom, BB);
      if (!TargetReachable.contains(&Target))
        return false;
    }
  }

  // Each CFG neighbor must have a direct same-kind proof edge from ProofRoot.
  // From that quotient boundary, expand only through proof edges that still
  // represent the same neighbor block.
  SourceGroups Groups;
  if (!collectSourceGroups(G, ProofRoot, Neighbors, Kind, Observations,
                           Groups))
    return false;

  // Seed from the first group, reject values unavailable at the target anchor,
  // then compute the exact object/location/kind intersection of all groups.
  for (Observation *Obs : Groups.front()) {
    for (MemoryAccess &Candidate : *Obs) {
      if (!Candidate.isMovable())
        continue;
      if (!isAccessAvailableAt(Candidate, Target, DT))
        continue;
      SmallVector<MemoryAccess *, 4> Matches = {&Candidate};
      bool Common = true;
      for (ArrayRef<Observation *> Group : drop_begin(Groups)) {
        MemoryAccess *Match = findMatchingAccess(Group, Candidate);
        if (!Match) {
          Common = false;
          break;
        }
        Matches.push_back(Match);
      }
      if (!Common)
        continue;

      commitCommonMotion(Target, Matches, Direction);

      // Move one common access per call; the outer fixed-point loop restarts
      // the scan with fresh iterators and exposes cascaded opportunities.
      return true;
    }
  }
  return false;
}

bool moveCommonAccessesToFixedPoint(MAIRGraph &G, MotionDirection Direction,
                                    DominatorTree &DT,
                                    const ObservationBlockIndex &Observations) {
  bool Changed = false;
  const Function *F = G.getFunction();
  assert(F && "CommonMAMotion requires a function-backed graph");
  while (true) {
    bool Moved = false;
    for (const BasicBlock &BB : *F) {
      if (!moveOneCommonAccess(G, BB, Direction, DT, Observations))
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
  const Function *F = G.getFunction();
  assert(F && "CommonMAMotion requires a function-backed graph");
  DominatorTree DT(*const_cast<Function *>(F));
  // Common motion only transfers accesses; its observation topology is stable.
  // Index represented blocks once and reuse the index across both fixed points.
  ObservationBlockIndex Observations(G);
  bool Changed = moveCommonAccessesToFixedPoint(G, MotionDirection::Hoist, DT,
                                                Observations);
  Observations.assertTopologyUnchanged(G);
  Changed |= moveCommonAccessesToFixedPoint(G, MotionDirection::Sink, DT,
                                            Observations);
  Observations.assertTopologyUnchanged(G);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
