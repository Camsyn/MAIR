#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class LoopInvariantObservationMotionPass
    : public llvm::PassInfoMixin<LoopInvariantObservationMotionPass> {
  bool MergeEquivalentObservations;
  unsigned Ordinal;

public:
  explicit LoopInvariantObservationMotionPass(
      bool MergeEquivalentObservations = true, unsigned Ordinal = 0)
      : MergeEquivalentObservations(MergeEquivalentObservations),
        Ordinal(Ordinal) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
