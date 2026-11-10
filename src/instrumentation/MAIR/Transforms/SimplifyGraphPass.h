#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

class SimplifyGraphPass : public llvm::PassInfoMixin<SimplifyGraphPass> {
  unsigned Ordinal;

public:
  explicit SimplifyGraphPass(unsigned Ordinal = 0) : Ordinal(Ordinal) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
