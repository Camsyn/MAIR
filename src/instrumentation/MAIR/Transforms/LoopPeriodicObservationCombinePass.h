#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class LoopPeriodicObservationCombinePass
    : public llvm::PassInfoMixin<LoopPeriodicObservationCombinePass> {
  bool MergeEquivalentObservations;
  unsigned Ordinal;

public:
  explicit LoopPeriodicObservationCombinePass(
      bool MergeEquivalentObservations = true, unsigned Ordinal = 0)
      : MergeEquivalentObservations(MergeEquivalentObservations),
        Ordinal(Ordinal) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
