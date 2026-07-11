#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class LocationMergePass : public llvm::PassInfoMixin<LocationMergePass> {
public:
  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
