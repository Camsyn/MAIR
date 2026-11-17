#include "MAIR/MAIRBarrierAnalysis.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"

using namespace llvm;

namespace mair {

bool isSanitizerCoverageCallback(const CallBase &CB) {
  const Function *Callee = CB.getCalledFunction();
  if (!Callee)
    return false;
  StringRef Name = Callee->getName();
  return Name.starts_with("__sanitizer_cov_trace_") ||
         Name.starts_with("__sanitizer_cov_load") ||
         Name.starts_with("__sanitizer_cov_store");
}

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
//
// Both cases together: let G - W be the CFG without the witness block W,
// with its edges reversed for the backward direction. A covered block C has a
// barrier path iff a block B with a barrier reaches C by a nonempty path in
// G - W. For B != C, B runs between W and C; B == C is the circuit through C.
// Above, backward, H reaches itself through A in G - B.
//
// The graph builder asks for every block, and blocks with the same witness
// share their searches. In a large switch, every block where cases join has
// the switch block as its immediate dominator and lies behind most of the
// cases, so one search per covered block is quadratic. One call instead
// answers all covered blocks of one witness in two linear passes over the
// region R, the blocks that reach a covered block in G - W:
//   1. depth-first searches against the edges from all covered blocks, which
//      treat W as visited, find R;
//   2. a flood along the edges from the successors of the barrier blocks in R
//      marks the blocks of R that a barrier block reaches by a nonempty path.
// Every block of a path from a barrier to a covered block reaches that block,
// so the flood need not leave R, and the marked covered blocks are the ones
// with a barrier path.
//
// The searches need not pass a barrier block that is not covered, either. If
// a barrier reaches a covered block C, so does the last barrier block on that
// path before C, and the blocks in between have no barrier. So the searches
// still find the part of the path that the flood needs. Where barriers are
// dense, as calls are for TSan, this keeps R close to the covered blocks.
//
// The cost is O(|R| + edges within R) per witness instead of the sum of the
// regions of the individual covered blocks.
BitVector
MAIRBarrierAnalysisCore::hasBarrierPaths(const BasicBlock &Witness,
                                         ArrayRef<const BasicBlock *> Covered,
                                         bool Forward) {
  SmallPtrSet<const BasicBlock *, 8> CoveredSet(llvm::from_range, Covered);
  df_iterator_default_set<const BasicBlock *> Region;
  Region.insert(&Witness);
  auto Search = [&](auto Blocks) {
    for (auto It = Blocks.begin(), End = Blocks.end(); It != End;) {
      if (!CoveredSet.contains(*It) && isBarrierBlock(**It, Forward))
        It.skipChildren();
      else
        ++It;
    }
  };
  for (const BasicBlock *BB : Covered) {
    if (Forward)
      Search(inverse_depth_first_ext(BB, Region));
    else
      Search(depth_first_ext(BB, Region));
  }

  SmallPtrSet<const BasicBlock *, 32> AfterBarrier;
  SmallVector<const BasicBlock *, 32> Worklist;
  auto MarkNext = [&](const BasicBlock *BB) {
    auto Mark = [&](const BasicBlock *Next) {
      if (Next != &Witness && Region.contains(Next) &&
          AfterBarrier.insert(Next).second)
        Worklist.push_back(Next);
    };
    if (Forward)
      for_each(successors(BB), Mark);
    else
      for_each(predecessors(BB), Mark);
  };
  for (const BasicBlock *BB : Region)
    if (BB != &Witness && isBarrierBlock(*BB, Forward))
      MarkNext(BB);
  while (!Worklist.empty())
    MarkNext(Worklist.pop_back_val());

  BitVector Result(Covered.size());
  for (auto [I, BB] : enumerate(Covered))
    if (AfterBarrier.contains(BB))
      Result.set(I);
  return Result;
}

} // namespace mair
