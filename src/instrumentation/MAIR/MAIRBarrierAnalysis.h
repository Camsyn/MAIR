#pragma once

#include "llvm/ADT/SetVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
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
  bool hasFwdBarrierBetween(const Instruction *From,
                            const Instruction *To) const;
  bool hasBwdBarrierBetween(const Instruction *From,
                            const Instruction *To) const;
  bool hasFwdBarrierPath(const BasicBlock &From, const BasicBlock &To) const;
  bool hasBwdBarrierPath(const BasicBlock &From, const BasicBlock &To) const;

private:
  mutable SmallSetVector<const BasicBlock *, 16> FwdBarrierBBMemo;
  mutable SmallSetVector<const BasicBlock *, 16> BwdBarrierBBMemo;

protected:
  virtual bool hasAnyBarrier(const BasicBlock &BB, bool Forward) const = 0;
  virtual bool hasAnyBarrier(const Instruction *From, const Instruction *To,
                             bool Forward) const = 0;

private:
  void cacheBarrierBB(const BasicBlock &BB, bool Forward) const {
    (Forward ? FwdBarrierBBMemo : BwdBarrierBBMemo).insert(&BB);
  }
  bool isInBarrierMemo(const BasicBlock &BB, bool Forward) const {
    return (Forward ? FwdBarrierBBMemo : BwdBarrierBBMemo).contains(&BB);
  }
  bool isBarrierBlock(const BasicBlock &BB, bool Foward) const;
  bool hasBarrierBetween(const Instruction *From, const Instruction *To,
                         bool Forward) const;
  bool hasBarrierPath(const BasicBlock &From, const BasicBlock &To,
                      bool Forward) const;
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

  bool hasAnyBarrier(const Instruction *From, const Instruction *To,
                     bool Forward) const override {
    assert(From && To && From->getParent());
    const BasicBlock &BB = *From->getParent();
    assert(&BB == To->getParent());
    assert(From != To && From->comesBefore(To));
    auto Rng = make_range(std::next(From->getIterator()), To->getIterator());
    return any_of(Rng, Forward ? BarrierTrait::isForwardBarrier
                               : BarrierTrait::isBackwardBarrier);
  }
};

} // namespace mair
