#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"

namespace mair {

class AvailableObservationSet {
  llvm::SmallSetVector<const Observation *, 8> Sources;

public:
  bool insert(const Observation &Obs);
  bool covers(const Observation &Obs,
              const llvm::DenseSet<const Observation *> &ExcludedSources) const;
};

class RedundantObservationInfo {
  llvm::DenseMap<const ProgramPoint *, AvailableObservationSet> InSets;

public:
  /// Deletable facts are a may-union over sources reachable through every
  /// typed edge enabled by the graph configuration, not a CFG intersection.
  const AvailableObservationSet &getIn(const ProgramPoint &PP) const;

private:
  friend class RedundantObservationAnalysis;
};

class RedundantObservationAnalysis
    : public llvm::AnalysisInfoMixin<RedundantObservationAnalysis> {
  friend llvm::AnalysisInfoMixin<RedundantObservationAnalysis>;
  static llvm::AnalysisKey Key;

public:
  using Result = RedundantObservationInfo;
  Result run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
