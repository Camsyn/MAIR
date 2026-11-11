#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallVector.h"

using namespace llvm;

namespace mair {

AnalysisKey RedundantObservationAnalysis::Key;

bool RedundantObservationInfo::isCovered(
    const Observation &Obs,
    const DenseSet<const Observation *> &ExcludedSources) const {
  const ProgramPoint *Target = Obs.getParent();
  const SparseBitVector<> &Sources =
      ReachingSources[ComponentOf[PointIndex.find(Target)->second]];
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
  if (!G.getOptimizationConfig().allowsCoverageAlong(Edge.getKind()))
    return false;
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

  // The extra node precedes every program point in the reversed graph, so one
  // traversal from it visits all of them.
  std::vector<ReachNode> Nodes(NumPoints + 1);
  ReachNode &Root = Nodes[NumPoints];
  BitVector IsSource(NumPoints);
  Index = 0;
  for (const ProgramPoint &PP : G) {
    ReachNode &Node = Nodes[Index];
    // A guarded program point may not execute, so it cannot be a witness.
    if (PP.hasAlwaysGuard() && !PP.empty())
      IsSource.set(Index);
    // Edge kinds are filtered at every hop because sanitizer configurations
    // may allow Dom coverage while rejecting PostDom coverage.
    for (const ProgramPointEdge &Edge : PP.children())
      if (canPropagate(G, PP, Edge))
        Nodes[Result.PointIndex.lookup(&Edge.getNode())].Preds.push_back(&Node);
    Root.Preds.push_back(&Node);
    ++Index;
  }

  // Components of the reversed graph come out with predecessors first, so the
  // set of every predecessor outside a component is final when it is read.
  Result.ComponentOf.assign(NumPoints, 0);
  for (auto SCC = scc_begin(&Root); !SCC.isAtEnd(); ++SCC) {
    if (SCC->front() == &Root)
      continue;
    const unsigned Component = Result.ReachingSources.size();
    for (const ReachNode *Node : *SCC)
      Result.ComponentOf[Node - Nodes.data()] = Component;

    SparseBitVector<> Sources;
    const bool HasCycle = SCC.hasCycle();
    for (const ReachNode *Node : *SCC) {
      const unsigned Point = Node - Nodes.data();
      // A local observation becomes available only after its program point,
      // so a program point reaches itself only around a cycle.
      if (HasCycle && IsSource.test(Point))
        Sources.set(Point);
      for (const ReachNode *PredNode : Node->Preds) {
        const unsigned Pred = PredNode - Nodes.data();
        const unsigned PredComponent = Result.ComponentOf[Pred];
        if (PredComponent == Component)
          continue;
        Sources |= Result.ReachingSources[PredComponent];
        if (IsSource.test(Pred))
          Sources.set(Pred);
      }
    }
    Result.ReachingSources.push_back(std::move(Sources));
  }

  return Result;
}

} // namespace mair
