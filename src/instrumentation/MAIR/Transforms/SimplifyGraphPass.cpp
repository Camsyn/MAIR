#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRStatLog.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

#include <set>

using namespace llvm;

#define DEBUG_TYPE "mair-simplifygraph"

STATISTIC(NumInputProgramPoints,
          "Number of program points entering graph simplification");
STATISTIC(NumMergedProgramPoints,
          "Number of program points merged by graph simplification");

namespace mair {

namespace {

struct MergeCandidate {
  ProgramPoint *Into = nullptr;
  ProgramPoint *From = nullptr;
};

bool hasPinnedObservation(const ProgramPoint &PP) {
  return llvm::any_of(PP,
                      [&](const Observation &Obs) { return !Obs.isMovable(); });
}

// The pair \p PP1 offers: its first Dom child that post-dominates it back.
MergeCandidate findMergeCandidate(ProgramPoint &PP1, const MAIRGraph &G) {
  if (isa<LoopProgramPoint>(PP1) || hasPinnedObservation(PP1))
    return {};
  for (const ProgramPointEdge &Edge : PP1.children()) {
    if (!Edge.isDom())
      continue;

    ProgramPoint &PP2 = Edge.getNode();
    if (&PP1 == &PP2 || isa<LoopProgramPoint>(PP2) ||
        !PP1.hasCompatibleMergeSemantics(PP2) ||
        !PP2.hasChild(PP1, PPEdgeKind::PostDom))
      continue;
    const BasicBlock *PP1Block = PP1.getBasicBlock();
    const BasicBlock *PP2Block = PP2.getBasicBlock();
    if (PP1Block != PP2Block) {
      assert(PP1Block && PP2Block &&
             "compatible program points must use the same site category");
      // Same constraint as PostDom redundancy: a loop-carried pointer can
      // take a new address on the exit, as in `load %p` inside the loop
      // and `load %p` after it. Do not merge those program points.
      const LoopInfo &Loops = G.getLoopInfo();
      if (Loops.getLoopFor(PP1Block) != Loops.getLoopFor(PP2Block))
        continue;
    }
    // Retain the post-dominating site so contraction sinks observations.
    return {&PP2, &PP1};
  }
  return {};
}

} // namespace

PreservedAnalyses SimplifyGraphPass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &) {
  MAIRStatScope Scope("simplify", Ordinal, G, NumInputProgramPoints, [&] {
    return MAIRStat::compute(G).getNumProgramPoints();
  });
  bool Changed = false;

  // Contract one mutual Dom/PostDom pair at a time, always the one of the
  // first program point in list order that offers one. Rewiring can expose a
  // new pair outside the previous neighborhood, so rescanning the graph after
  // every contraction would find the next pair, but is quadratic on large
  // functions.
  //
  // Whether P offers a pair depends only on the observations and children of
  // P and on which Dom children of P have a PostDom edge back to P.
  // Contracting From into Into changes the observations and children of Into
  // and the children of the parents of From. Among the PostDom edges, only
  // Into gains some, those From had. So besides these program points, only
  // the Dom parents of Into can change their answer:
  //
  //   X -Dom-> I               contracting F into I gives I -PostDom-> X,
  //   F -PostDom-> X           so X now offers (X, I)
  //   F -Dom-> I -PostDom-> F
  //
  // One scan in list order then suffices if it takes these program points up
  // again when it has passed them, before it goes on. Each contraction costs
  // time in the size of its neighborhood instead of a rescan of the graph.
  SmallVector<ProgramPoint *, 0> Points(make_pointer_range(G));
  DenseMap<const ProgramPoint *, unsigned> Position;
  for (auto [I, PP] : enumerate(Points))
    Position[PP] = I;
  unsigned Next = 0;
  // Positions before Next whose answer may have changed.
  std::set<unsigned> Revisit;
  auto FindCandidate = [&]() -> MergeCandidate {
    while (!Revisit.empty()) {
      ProgramPoint *PP = Points[*Revisit.begin()];
      Revisit.erase(Revisit.begin());
      if (MergeCandidate Candidate = findMergeCandidate(*PP, G); Candidate.Into)
        return Candidate;
    }
    for (; Next < Points.size(); ++Next)
      if (ProgramPoint *PP = Points[Next])
        if (MergeCandidate Candidate = findMergeCandidate(*PP, G);
            Candidate.Into)
          return Candidate;
    return {};
  };
  while (true) {
    MergeCandidate Candidate = FindCandidate();
    if (!Candidate.Into)
      break;
    ProgramPoint &Into = *Candidate.Into;
    SmallVector<ProgramPoint *, 8> Rewired{&Into};
    for (const ProgramPointEdge &Edge : Candidate.From->parents())
      if (&Edge.getNode() != Candidate.From)
        Rewired.push_back(&Edge.getNode());
    Points[Position.lookup(Candidate.From)] = nullptr;
    G.contractProgramPoint(Into, *Candidate.From);
    ++NumMergedProgramPoints;
    MAIRStatScope::add(MAIRStatField::Merged);
    Changed = true;

    auto Recheck = [&](const ProgramPoint &PP) {
      unsigned I = Position.lookup(&PP);
      if (I < Next)
        Revisit.insert(I);
    };
    for (ProgramPoint *PP : Rewired)
      Recheck(*PP);
    for (const ProgramPointEdge &Edge : Into.parents())
      if (Edge.isDom())
        Recheck(Edge.getNode());
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
