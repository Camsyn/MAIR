#include "MAIR/MAIRBarrierAnalysis.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"

using namespace llvm;

namespace mair {

bool MAIRBarrierAnalysisCore::isBarrierBlock(const BasicBlock &BB,
                                             bool Forward) const {
  if (isInBarrierMemo(BB, Forward))
    return true;
  const bool Result = hasAnyBarrier(BB, Forward);
  if (Result)
    cacheBarrierBB(BB, Forward);
  return Result;
}

bool MAIRBarrierAnalysisCore::hasFwdBarrierBetween(
    const Instruction *ComesBefore, const Instruction *ComesAfter) const {
  return hasBarrierBetween(ComesBefore, ComesAfter, true);
}

bool MAIRBarrierAnalysisCore::hasBwdBarrierBetween(
    const Instruction *ComesBefore, const Instruction *ComesAfter) const {
  return hasBarrierBetween(ComesBefore, ComesAfter, false);
}

bool MAIRBarrierAnalysisCore::hasBarrierBetween(const Instruction *ComesBefore,
                                                const Instruction *ComesAfter,
                                                bool Forward) const {
  bool Result = hasAnyBarrier(ComesBefore, ComesAfter, Forward);
  if (Result)
    cacheBarrierBB(*ComesBefore->getParent(), Forward);
  return Result;
}

bool MAIRBarrierAnalysisCore::hasBarrierPath(const BasicBlock &From,
                                             const BasicBlock &To,
                                             bool Forward) const {
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
                                                const BasicBlock &To) const {
  return hasBarrierPath(From, To, true);
}

bool MAIRBarrierAnalysisCore::hasBwdBarrierPath(const BasicBlock &From,
                                                const BasicBlock &To) const {
  return hasBarrierPath(From, To, false);
}

} // namespace mair
