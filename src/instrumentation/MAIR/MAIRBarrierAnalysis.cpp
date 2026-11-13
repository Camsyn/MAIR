#include "MAIR/MAIRBarrierAnalysis.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"

using namespace llvm;

namespace mair {

bool MAIRBarrierAnalysisCore::isBarrierBlock(const BasicBlock &BB,
                                             bool Forward) {
  auto &Memo = Forward ? FwdBarrierBBMemo : BwdBarrierBBMemo;
  auto [It, Inserted] = Memo.try_emplace(&BB, false);
  if (Inserted)
    It->second = hasAnyBarrier(BB, Forward);
  return It->second;
}

bool MAIRBarrierAnalysisCore::hasBarrierBetween(const BasicBlock &BB,
                                                const Instruction *From,
                                                const Instruction *To,
                                                bool Forward) {
  if (From && From == To)
    return false;
  if (!hasAnyBarrier(BB, From, To, Forward))
    return false;
  (Forward ? FwdBarrierBBMemo : BwdBarrierBBMemo)[&BB] = true;
  return true;
}

// Whether a barrier can run between the witness and the observation it
// covers. Forward, From dominates To and the witness is at the end of From;
// backward, To post-dominates From and the witness is at the start of To.
//
// A path counts from the last run of the witness block, so the search stops
// there. The covered block has no such limit:
//
//   H:  call void @barrier()            ; From
//       store i32 0, ptr %p
//       br i1 %c, label %A, label %B
//   B:  store i32 1, ptr %p             ; To, post-dominates H
//       br i1 %done, label %exit, label %A
//   A:  br label %H
//
// H can run again before B does. The call then lies between the earlier
// store in H and the store in B, although it is in neither block "between"
// them. So when a circuit through the covered block avoids the witness
// block, the covered block counts as well.
bool MAIRBarrierAnalysisCore::hasBarrierPath(const BasicBlock &From,
                                             const BasicBlock &To,
                                             bool Forward) {
  const BasicBlock &Witness = Forward ? From : To;
  const BasicBlock &Covered = Forward ? To : From;
  df_iterator_default_set<const BasicBlock *> Visited;
  Visited.insert(&Witness);
  auto IsBarrier = [&](const BasicBlock *BB) {
    return BB != &From && BB != &To && isBarrierBlock(*BB, Forward);
  };
  if (Forward ? any_of(inverse_depth_first_ext(&To, Visited), IsBarrier)
              : any_of(depth_first_ext(&From, Visited), IsBarrier))
    return true;

  // Visited now holds every block on a path between the two ends.
  auto ClosesCircuit = [&](const BasicBlock *Next) {
    return Next != &Witness && Visited.count(Next);
  };
  const bool RunsAgain = Forward
                             ? any_of(successors(&Covered), ClosesCircuit)
                             : any_of(predecessors(&Covered), ClosesCircuit);
  return RunsAgain && isBarrierBlock(Covered, Forward);
}

} // namespace mair
