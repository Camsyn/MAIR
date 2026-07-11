#pragma once

#include "MAIR/MAIRPasses.h"

namespace mair {

class RedundantObservationEliminationPass
    : public llvm::PassInfoMixin<RedundantObservationEliminationPass> {
public:
  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
