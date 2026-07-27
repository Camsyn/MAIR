#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"

namespace mair {

class AvailableAccessSet {
  llvm::SmallSetVector<const MemoryAccess *, 8> Sources;

public:
  bool insert(const MemoryAccess &Access);
  bool
  covers(const MemoryAccess &Access,
         const llvm::DenseSet<const MemoryAccess *> &ExcludedSources) const;
};

class RedundantObservationInfo {
  llvm::DenseMap<const Observation *, AvailableAccessSet> InSets;

public:
  /// Deletable facts are a may-union over sources reachable through every
  /// typed edge enabled by the graph configuration, not a CFG intersection.
  const AvailableAccessSet &getIn(const Observation &Obs) const;

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
