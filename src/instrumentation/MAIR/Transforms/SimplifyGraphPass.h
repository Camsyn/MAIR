#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class SimplifyGraphPass : public llvm::PassInfoMixin<SimplifyGraphPass> {
public:
  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
