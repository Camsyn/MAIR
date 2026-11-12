#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallVector.h"

using namespace llvm;

namespace mair {

AnalysisKey RedundantObservationAnalysis::Key;

// Edges that iterate a cycle beyond this many distinct ones stay closed.
static constexpr unsigned MaxTrackedCycles = 64;

uint64_t RedundantObservationInfo::closedCycles(const Observation &Obs) const {
  uint64_t Closed = 0;
  for (unsigned I = 0; I < IteratedCycles.size(); ++I)
    if (!Obs.isInvariantIn(*IteratedCycles[I]))
      Closed |= uint64_t(1) << I;
  return Closed;
}

bool RedundantObservationInfo::isCovered(
    const Observation &Obs,
    const DenseSet<const Observation *> &ExcludedSources) const {
  const ProgramPoint *Target = Obs.getParent();
  // A covering observation names the same bytes through the same values, so
  // the cycles that are closed to Obs are closed to it as well.
  const Reachability &Reach = ByClosedCycles.find(closedCycles(Obs))->second;
  const SparseBitVector<> &Sources =
      Reach.ReachingSources[Reach.ComponentOf[PointIndex.find(Target)->second]];
  if (Sources.empty())
    return false;
  // Only an observation of the same memory object can cover Obs. The use-list
  // also holds observations of other scopes, which have no index here.
  for (const Observation &Source : Obs.getMemoryObject().users()) {
    const ProgramPoint *SourcePoint = Source.getParent();
    if (SourcePoint == Target)
      continue;
    auto It = PointIndex.find(SourcePoint);
    if (It == PointIndex.end() || !Sources.test(It->second))
      continue;
    if (!ExcludedSources.contains(&Source) && Source.covers(Obs))
      return true;
  }
  return false;
}

namespace {

// A program point in the graph of enabled edges, with the edges reversed.
struct ReachNode {
  SmallVector<ReachNode *, 4> Preds;
};

} // namespace

} // namespace mair

namespace llvm {

template <> struct GraphTraits<mair::ReachNode *> {
  using NodeRef = mair::ReachNode *;
  using ChildIteratorType = SmallVectorImpl<mair::ReachNode *>::iterator;

  static NodeRef getEntryNode(NodeRef Node) { return Node; }
  static ChildIteratorType child_begin(NodeRef Node) {
    return Node->Preds.begin();
  }
  static ChildIteratorType child_end(NodeRef Node) { return Node->Preds.end(); }
};

} // namespace llvm

namespace mair {

namespace {

// PostDom coverage compares two uses of a value that dominates both. The
// value can denote different addresses when its definition and the earlier
// use are in one loop and the later use is outside that loop:
//
//   loop:
//     %p = getelementptr i32, ptr %base, i64 %i
//     %v = load i32, ptr %p        ; iteration 0 reads base[0]
//   exit:
//     %last = load i32, ptr %p     ; reads base[n-1]
//
// A PostDom edge may connect their program points. The observations are
// redundant only when both points are in the same loop. Two blocks outside
// every loop also match.
bool sameLoopForPostDom(const ProgramPoint &From, const ProgramPoint &To,
                        const llvm::LoopInfo &LI) {
  const BasicBlock *FromBB = From.getBasicBlock();
  const BasicBlock *ToBB = To.getBasicBlock();
  if (!FromBB || !ToBB)
    return false;
  return LI.getLoopFor(FromBB) == LI.getLoopFor(ToBB);
}

bool canPropagate(const MAIRGraph &G, const ProgramPoint &From,
                  const ProgramPointEdge &Edge) {
  // Some unit-test graphs are built without analyses. A graph that has no
  // LoopInfo keeps the previous coverage rule.
  if (Edge.getKind() == PPEdgeKind::PostDom && G.hasLoopInfo() &&
      !sameLoopForPostDom(From, Edge.getNode(), G.getLoopInfo()))
    return false;
  return true;
}

} // namespace

RedundantObservationInfo
RedundantObservationAnalysis::run(MAIRGraph &G, MAIRGraphAnalysisManager &AM) {
  (void)AM;
  RedundantObservationInfo Result;
  const unsigned NumPoints = G.size();
  unsigned Index = 0;
  for (const ProgramPoint &PP : G)
    Result.PointIndex[&PP] = Index++;

  // An edge is identified by its ends here. Closed is the bit of the cycle it
  // iterates, if any.
  struct EnabledEdge {
    unsigned From, To;
    uint64_t Closed;
  };
  SmallVector<EnabledEdge, 32> Edges;
  BitVector IsSource(NumPoints);
  Index = 0;
  for (const ProgramPoint &PP : G) {
    // A guarded program point may not execute, so it cannot be a witness.
    if (PP.hasAlwaysGuard() && !PP.empty())
      IsSource.set(Index);
    for (const ProgramPointEdge &Edge : PP.children()) {
      if (!canPropagate(G, PP, Edge))
        continue;
      uint64_t Closed = 0;
      if (const Cycle *Iterated = Edge.getIteratedCycle()) {
        auto It = find(Result.IteratedCycles, Iterated);
        const unsigned CycleIndex = It - Result.IteratedCycles.begin();
        if (It == Result.IteratedCycles.end()) {
          if (CycleIndex >= MaxTrackedCycles)
            continue;
          Result.IteratedCycles.push_back(Iterated);
        }
        Closed = uint64_t(1) << CycleIndex;
      }
      Edges.push_back(
          {Index, Result.PointIndex.lookup(&Edge.getNode()), Closed});
    }
    ++Index;
  }

  auto Compute = [&](uint64_t Closed) {
    RedundantObservationInfo::Reachability Reach;
    // The extra node precedes every program point in the reversed graph, so
    // one traversal from it visits all of them.
    std::vector<ReachNode> Nodes(NumPoints + 1);
    ReachNode &Root = Nodes[NumPoints];
    for (const EnabledEdge &Edge : Edges)
      if (!(Edge.Closed & Closed))
        Nodes[Edge.To].Preds.push_back(&Nodes[Edge.From]);
    for (unsigned Point = 0; Point < NumPoints; ++Point)
      Root.Preds.push_back(&Nodes[Point]);

    // Components of the reversed graph come out with predecessors first, so
    // the set of every predecessor outside a component is final when read.
    Reach.ComponentOf.assign(NumPoints, 0);
    for (auto SCC = scc_begin(&Root); !SCC.isAtEnd(); ++SCC) {
      if (SCC->front() == &Root)
        continue;
      const unsigned Component = Reach.ReachingSources.size();
      for (const ReachNode *Node : *SCC)
        Reach.ComponentOf[Node - Nodes.data()] = Component;

      SparseBitVector<> Sources;
      const bool HasCycle = SCC.hasCycle();
      for (const ReachNode *Node : *SCC) {
        const unsigned Point = Node - Nodes.data();
        // A local observation becomes available only after its program
        // point, so a program point reaches itself only around a cycle.
        if (HasCycle && IsSource.test(Point))
          Sources.set(Point);
        for (const ReachNode *PredNode : Node->Preds) {
          const unsigned Pred = PredNode - Nodes.data();
          const unsigned PredComponent = Reach.ComponentOf[Pred];
          if (PredComponent == Component)
            continue;
          Sources |= Reach.ReachingSources[PredComponent];
          if (IsSource.test(Pred))
            Sources.set(Pred);
        }
      }
      Reach.ReachingSources.push_back(std::move(Sources));
    }
    return Reach;
  };

  for (const ProgramPoint &PP : G)
    for (const Observation &Obs : PP) {
      const uint64_t Closed = Result.closedCycles(Obs);
      if (!Result.ByClosedCycles.count(Closed))
        Result.ByClosedCycles.emplace(Closed, Compute(Closed));
    }
  return Result;
}

} // namespace mair
