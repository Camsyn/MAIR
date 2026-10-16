#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class LoopPeriodicObservationCombinePass
    : public llvm::PassInfoMixin<LoopPeriodicObservationCombinePass> {
  bool MergeEquivalentObservations;

public:
  explicit LoopPeriodicObservationCombinePass(
      bool MergeEquivalentObservations = true)
      : MergeEquivalentObservations(MergeEquivalentObservations) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
