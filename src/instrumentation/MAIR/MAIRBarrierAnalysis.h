#pragma once

#include "llvm/ADT/SetVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"

#include <cassert>
#include <iterator>

namespace mair {
using namespace llvm;

struct DefaultBarrierTrait {
  static bool isForwardBarrier(const Instruction &) { return false; }
  static bool isBackwardBarrier(const Instruction &) { return false; }
};

class MAIRBarrierAnalysisCore {
public:
  explicit MAIRBarrierAnalysisCore() {}
  /// Query barriers in one basic block interval.
  ///
  /// Null \p From means the interval starts at the block begin; null \p To
  /// means the interval ends at the block end. When both are present, \p From
  /// must strictly precede \p To and the interval excludes both endpoints.
  bool hasFwdBarrierBetween(const BasicBlock &BB, const Instruction *From,
                            const Instruction *To);
  bool hasBwdBarrierBetween(const BasicBlock &BB, const Instruction *From,
                            const Instruction *To);
  bool hasFwdBarrierBetween(const Instruction *From, const Instruction *To);
  bool hasBwdBarrierBetween(const Instruction *From, const Instruction *To);
  bool hasFwdBarrierPath(const BasicBlock &From, const BasicBlock &To);
  bool hasBwdBarrierPath(const BasicBlock &From, const BasicBlock &To);

private:
  SmallSetVector<const BasicBlock *, 16> FwdBarrierBBMemo;
  SmallSetVector<const BasicBlock *, 16> BwdBarrierBBMemo;

protected:
  virtual bool hasAnyBarrier(const BasicBlock &BB, bool Forward) const = 0;
  virtual bool hasAnyBarrier(const BasicBlock &BB, const Instruction *From,
                             const Instruction *To, bool Forward) const = 0;

private:
  void cacheBarrierBB(const BasicBlock &BB, bool Forward) {
    (Forward ? FwdBarrierBBMemo : BwdBarrierBBMemo).insert(&BB);
  }
  bool isInBarrierMemo(const BasicBlock &BB, bool Forward) const {
    return (Forward ? FwdBarrierBBMemo : BwdBarrierBBMemo).contains(&BB);
  }
  bool isBarrierBlock(const BasicBlock &BB, bool Foward);
  bool hasBarrierBetween(const BasicBlock &BB, const Instruction *From,
                         const Instruction *To, bool Forward);
  bool hasBarrierPath(const BasicBlock &From, const BasicBlock &To,
                      bool Forward);
};

template <class BarrierTrait = DefaultBarrierTrait>
class MAIRBarrierAnalysis final : public MAIRBarrierAnalysisCore {
public:
  explicit MAIRBarrierAnalysis() : MAIRBarrierAnalysisCore() {}

protected:
  bool hasAnyBarrier(const BasicBlock &BB, bool Forward) const override {
    return any_of(BB, Forward ? BarrierTrait::isForwardBarrier
                              : BarrierTrait::isBackwardBarrier);
  }

  bool hasAnyBarrier(const BasicBlock &BB, const Instruction *From,
                     const Instruction *To, bool Forward) const override {
    assert((!From || From->getParent() == &BB) &&
           "From must belong to the queried basic block");
    assert((!To || To->getParent() == &BB) &&
           "To must belong to the queried basic block");
    assert((!From || !To || (From != To && From->comesBefore(To))) &&
           "From must strictly precede To");

    auto Begin = From ? std::next(From->getIterator()) : BB.begin();
    auto End = To ? To->getIterator() : BB.end();
    return any_of(make_range(Begin, End), Forward
                                             ? BarrierTrait::isForwardBarrier
                                             : BarrierTrait::isBackwardBarrier);
  }
};

} // namespace mair
