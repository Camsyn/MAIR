#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class LoopPeriodicObservationCombinePass
    : public llvm::PassInfoMixin<LoopPeriodicObservationCombinePass> {
  bool MergeEquivalentAccesses;

public:
  explicit LoopPeriodicObservationCombinePass(
      bool MergeEquivalentAccesses = true)
      : MergeEquivalentAccesses(MergeEquivalentAccesses) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
