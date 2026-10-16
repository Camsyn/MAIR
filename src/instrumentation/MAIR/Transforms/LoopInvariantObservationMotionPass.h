#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class LoopInvariantObservationMotionPass
    : public llvm::PassInfoMixin<LoopInvariantObservationMotionPass> {
  bool MergeEquivalentObservations;

public:
  explicit LoopInvariantObservationMotionPass(
      bool MergeEquivalentObservations = true)
      : MergeEquivalentObservations(MergeEquivalentObservations) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
