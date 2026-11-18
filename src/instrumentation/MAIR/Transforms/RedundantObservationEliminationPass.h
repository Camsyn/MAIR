#pragma once

#include "MAIR/MAIRPasses.h"

namespace mair {

class RedundantObservationEliminationPass
    : public llvm::PassInfoMixin<RedundantObservationEliminationPass> {
  unsigned Ordinal;
  bool PartialCoverage;

public:
  explicit RedundantObservationEliminationPass(unsigned Ordinal = 0,
                                               bool PartialCoverage = false)
      : Ordinal(Ordinal), PartialCoverage(PartialCoverage) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
