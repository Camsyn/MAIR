#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"

namespace llvm {
class CallBase;
} // namespace llvm

#include <cassert>
#include <iterator>

namespace mair {

using llvm::any_of;
using llvm::ArrayRef;
using llvm::BasicBlock;
using llvm::BitVector;
using llvm::DenseMap;
using llvm::Instruction;
using llvm::make_range;

struct DefaultBarrierTrait {
  static bool isForwardBarrier(const Instruction &) { return false; }
  static bool isBackwardBarrier(const Instruction &) { return false; }
};

/// Whether \p CB calls one of the callbacks SanitizerCoverage puts at its
/// instrumentation sites: __sanitizer_cov_trace_*, __sanitizer_cov_load* and
/// __sanitizer_cov_store*. Their implementations (libFuzzer's
/// FuzzerTracePC.cpp, the weak defaults in sanitizer_common's
/// sanitizer_coverage_libcdep_new.cpp) only record coverage, are not
/// instrumented and do not synchronize, so they change neither ASan's shadow
/// nor TSan's state. The sanitizer barrier traits let them through in both
/// directions, as calls that return. See "Barrier" in docs/mair-loops.md.
bool isSanitizerCoverageCallback(const llvm::CallBase &CB);

class MAIRBarrierAnalysisCore {
public:
  /// Query barriers in one basic block interval.
  ///
  /// Null \p From means the interval starts at the block begin; null \p To
  /// means the interval ends at the block end. When both are present, \p From
  /// must strictly precede \p To and the interval excludes both endpoints.
  bool hasFwdBarrierBetween(const BasicBlock &BB, const Instruction *From,
                            const Instruction *To) {
    return hasBarrierBetween(BB, From, To, /*Forward=*/true);
  }
  bool hasBwdBarrierBetween(const BasicBlock &BB, const Instruction *From,
                            const Instruction *To) {
    return hasBarrierBetween(BB, From, To, /*Forward=*/false);
  }
  bool hasFwdBarrierPath(const BasicBlock &From, const BasicBlock &To) {
    return hasBarrierPaths(From, {&To}, /*Forward=*/true)[0];
  }
  bool hasBwdBarrierPath(const BasicBlock &From, const BasicBlock &To) {
    return hasBarrierPaths(To, {&From}, /*Forward=*/false)[0];
  }
  /// Batch forms that share one search per witness block: bit I of the result
  /// is hasFwdBarrierPath(From, *To[I]), respectively
  /// hasBwdBarrierPath(*From[I], To).
  BitVector hasFwdBarrierPaths(const BasicBlock &From,
                               ArrayRef<const BasicBlock *> To) {
    return hasBarrierPaths(From, To, /*Forward=*/true);
  }
  BitVector hasBwdBarrierPaths(ArrayRef<const BasicBlock *> From,
                               const BasicBlock &To) {
    return hasBarrierPaths(To, From, /*Forward=*/false);
  }
  bool hasFwdBarrier(const BasicBlock &BB) {
    return isBarrierBlock(BB, /*Forward=*/true);
  }
  bool hasBwdBarrier(const BasicBlock &BB) {
    return isBarrierBlock(BB, /*Forward=*/false);
  }
  bool isFwdBarrier(const Instruction &I) const {
    return isBarrier(I, /*Forward=*/true);
  }
  bool isBwdBarrier(const Instruction &I) const {
    return isBarrier(I, /*Forward=*/false);
  }

protected:
  virtual bool hasAnyBarrier(const BasicBlock &BB, bool Forward) const = 0;
  virtual bool hasAnyBarrier(const BasicBlock &BB, const Instruction *From,
                             const Instruction *To, bool Forward) const = 0;
  virtual bool isBarrier(const Instruction &I, bool Forward) const = 0;

private:
  DenseMap<const BasicBlock *, bool> FwdBarrierBBMemo;
  DenseMap<const BasicBlock *, bool> BwdBarrierBBMemo;

  bool isBarrierBlock(const BasicBlock &BB, bool Forward);
  bool hasBarrierBetween(const BasicBlock &BB, const Instruction *From,
                         const Instruction *To, bool Forward);
  BitVector hasBarrierPaths(const BasicBlock &Witness,
                            ArrayRef<const BasicBlock *> Covered, bool Forward);
};

template <class BarrierTrait = DefaultBarrierTrait>
class MAIRBarrierAnalysis final : public MAIRBarrierAnalysisCore {
protected:
  bool isBarrier(const Instruction &I, bool Forward) const override {
    return Forward ? BarrierTrait::isForwardBarrier(I)
                   : BarrierTrait::isBackwardBarrier(I);
  }

  bool hasAnyBarrier(const BasicBlock &BB, bool Forward) const override {
    return any_of(BB,
                  [&](const Instruction &I) { return isBarrier(I, Forward); });
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
    return any_of(make_range(Begin, End),
                  [&](const Instruction &I) { return isBarrier(I, Forward); });
  }
};

} // namespace mair
