#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace mair {

struct AvailableAccess {
  const MemoryAccess *Source = nullptr;
  const Observation *SourceObservation = nullptr;

  bool covers(const MemoryAccess &Other) const;
  bool operator==(const AvailableAccess &Other) const;
};

class AvailableAccessSet {
  llvm::SmallVector<AvailableAccess, 8> Accesses;

public:
  bool insert(const AvailableAccess &Access);
  bool unionWith(const AvailableAccessSet &Other);
  bool covers(const MemoryAccess &Access) const;
  bool
  covers(const MemoryAccess &Access,
         const llvm::DenseSet<const MemoryAccess *> &ExcludedSources) const;
  bool operator==(const AvailableAccessSet &Other) const;
  bool operator!=(const AvailableAccessSet &Other) const {
    return !(*this == Other);
  }
};

class RedundantObservationInfo {
  llvm::DenseMap<const Observation *, AvailableAccessSet> InSets;
  llvm::DenseMap<const Observation *, AvailableAccessSet> OutSets;

public:
  const AvailableAccessSet &getIn(const Observation &Obs) const;
  const AvailableAccessSet &getOut(const Observation &Obs) const;

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
