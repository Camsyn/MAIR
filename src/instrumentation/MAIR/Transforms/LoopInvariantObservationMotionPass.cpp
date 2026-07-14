#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace mair {

namespace {

bool isSameAccess(const MemoryAccess &LHS, const MemoryAccess &RHS) {
  return &LHS.getMemoryObject() == &RHS.getMemoryObject() &&
         LHS.getRegion() == RHS.getRegion() && LHS.getKind() == RHS.getKind();
}

void eraseAccess(MemoryAccess &Access) {
  Observation &Parent = *Access.getParent();
  auto It = llvm::find_if(Parent.accesses(), [&](MemoryAccess &Candidate) {
    return &Candidate == &Access;
  });
  assert(It != Parent.end() && "memory access must belong to its parent");
  Parent.erase(It);
}

void moveAccessTo(MemoryAccess &Access, Observation &Target) {
  MemoryAccess *Accumulator = nullptr;
  for (MemoryAccess &Existing : Target)
    if (isSameAccess(Existing, Access)) {
      Accumulator = &Existing;
      break;
    }

  if (!Accumulator) {
    Target.addAccess(Access);
  } else {
    bool Merged = Accumulator->mergeWith(Access);
    assert(Merged && "exactly matching accesses must be mergeable");
    (void)Merged;
  }
  eraseAccess(Access);
}

bool moveOneInvariantAccess(MAIRGraph &G, MAIRLoopInfo &Loops) {
  if (!Loops.isAvailable())
    return false;

  for (Observation &Obs : G) {
    Loop *L = Loops.getLoopFor(Obs);
    if (!L || !Loops.isSimpleLoop(*L))
      continue;

    for (MemoryAccess &Access : Obs) {
      // First prove that the access executes on every loop entry and that its
      // complete MAIR location is invariant in the highest enclosing simple
      // loop. Only then move it to that loop's existing preheader point.
      if (!Loops.isEveryIterationAccess(Access, *L))
        continue;
      if (!Loops.isLoopInvariant(Access, *L))
        continue;

      Loop *Top = Loops.getTopInvariantLoop(Access, *L);
      if (!Top)
        continue;
      if (!Loops.isGuaranteedOnLoopEntry(Access, *Top))
        continue;

      Observation *Target = Loops.getPreheaderExit(*Top);
      if (!Target || Target == &Obs)
        continue;

      // CFG-changing exit and indicator cases are intentionally not modeled by
      // this graph-only pass; they require the later instantiate layer.
      moveAccessTo(Access, *Target);
      return true;
    }
  }
  return false;
}

} // namespace

PreservedAnalyses
LoopInvariantObservationMotionPass::run(MAIRGraph &G,
                                        MAIRGraphAnalysisManager &AM) {
  bool Changed = false;
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  while (moveOneInvariantAccess(G, Loops))
    Changed = true;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
