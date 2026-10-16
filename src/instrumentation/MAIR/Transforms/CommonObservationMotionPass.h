#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class CommonObservationMotionPass
    : public llvm::PassInfoMixin<CommonObservationMotionPass> {
public:
  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
