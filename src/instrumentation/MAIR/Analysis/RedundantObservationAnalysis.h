#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SparseBitVector.h"

#include <vector>

namespace mair {

/// Which program points can witness the observations of which others.
///
/// A source program point reaches a target when a path of one or more typed
/// edges enabled by the graph configuration leads from it to the target. This
/// is a may-union over paths, not a CFG intersection. Program points in one
/// strongly connected component are reached by the same sources, so the sets
/// are stored per component.
class RedundantObservationInfo {
  llvm::DenseMap<const ProgramPoint *, unsigned> PointIndex;
  std::vector<unsigned> ComponentOf;
  std::vector<llvm::SparseBitVector<>> ReachingSources;

public:
  /// Whether an observation that is not in \p ExcludedSources, and whose
  /// program point reaches the program point of \p Obs, covers \p Obs.
  bool
  isCovered(const Observation &Obs,
            const llvm::DenseSet<const Observation *> &ExcludedSources) const;

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
