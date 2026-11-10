#pragma once

#include "MAIR/MAIRPasses.h"

namespace mair {

class RedundantObservationEliminationPass
    : public llvm::PassInfoMixin<RedundantObservationEliminationPass> {
  unsigned Ordinal;

public:
  explicit RedundantObservationEliminationPass(unsigned Ordinal = 0)
      : Ordinal(Ordinal) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
