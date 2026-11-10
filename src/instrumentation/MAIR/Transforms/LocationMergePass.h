#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class LocationMergePass : public llvm::PassInfoMixin<LocationMergePass> {
  unsigned Ordinal;

public:
  explicit LocationMergePass(unsigned Ordinal = 0) : Ordinal(Ordinal) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
