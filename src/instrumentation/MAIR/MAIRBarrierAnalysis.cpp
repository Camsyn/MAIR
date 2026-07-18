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
  if (auto It = Memo.find(&BB); It != Memo.end())
    return It->second;
  const bool Result = hasAnyBarrier(BB, Forward);
  Memo[&BB] = Result;
  return Result;
}

bool MAIRBarrierAnalysisCore::hasFwdBarrierBetween(
    const BasicBlock &BB, const Instruction *ComesBefore,
    const Instruction *ComesAfter) {
  return hasBarrierBetween(BB, ComesBefore, ComesAfter, true);
}

bool MAIRBarrierAnalysisCore::hasBwdBarrierBetween(
    const BasicBlock &BB, const Instruction *ComesBefore,
    const Instruction *ComesAfter) {
  return hasBarrierBetween(BB, ComesBefore, ComesAfter, false);
}

bool MAIRBarrierAnalysisCore::hasFwdBarrierBetween(
    const Instruction *ComesBefore, const Instruction *ComesAfter) {
  assert(ComesBefore && ComesAfter && ComesBefore->getParent() &&
         ComesAfter->getParent() == ComesBefore->getParent());
  return hasBarrierBetween(*ComesBefore->getParent(), ComesBefore, ComesAfter,
                           true);
}

bool MAIRBarrierAnalysisCore::hasBwdBarrierBetween(
    const Instruction *ComesBefore, const Instruction *ComesAfter) {
  assert(ComesBefore && ComesAfter && ComesBefore->getParent() &&
         ComesAfter->getParent() == ComesBefore->getParent());
  return hasBarrierBetween(*ComesBefore->getParent(), ComesBefore, ComesAfter,
                           false);
}

bool MAIRBarrierAnalysisCore::hasBarrierBetween(const BasicBlock &BB,
                                                const Instruction *ComesBefore,
                                                const Instruction *ComesAfter,
                                                bool Forward) {
  if (ComesBefore && ComesAfter && ComesBefore == ComesAfter)
    return false;
  bool Result = hasAnyBarrier(BB, ComesBefore, ComesAfter, Forward);
  if (Result)
    (Forward ? FwdBarrierBBMemo : BwdBarrierBBMemo)[&BB] = true;
  return Result;
}

bool MAIRBarrierAnalysisCore::hasBarrierPath(const BasicBlock &From,
                                             const BasicBlock &To,
                                             bool Forward) {
  // Assertion:
  //   From dom To || To pdom From
  df_iterator_default_set<const BasicBlock *> End;
  End.insert(Forward ? &From : &To);
  auto Predicate = [&](const BasicBlock *BB) {
    return BB != &From && BB != &To && this->isBarrierBlock(*BB, Forward);
  };
  return Forward ? /* From dom To  */ any_of(inverse_depth_first_ext(&To, End),
                                             Predicate)
                 : /* To pdom From */ any_of(depth_first_ext(&From, End),
                                             Predicate);
}

bool MAIRBarrierAnalysisCore::hasFwdBarrierPath(const BasicBlock &From,
                                                const BasicBlock &To) {
  return hasBarrierPath(From, To, true);
}

bool MAIRBarrierAnalysisCore::hasBwdBarrierPath(const BasicBlock &From,
                                                const BasicBlock &To) {
  return hasBarrierPath(From, To, false);
}

} // namespace mair
