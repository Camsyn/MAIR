#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class CommonObservationMotionPass
    : public llvm::PassInfoMixin<CommonObservationMotionPass> {
  unsigned Ordinal;

public:
  explicit CommonObservationMotionPass(unsigned Ordinal = 0)
      : Ordinal(Ordinal) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
