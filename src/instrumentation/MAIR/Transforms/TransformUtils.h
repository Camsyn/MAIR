#pragma once

#include "MAIR/MAIR.h"

namespace mair {

/// Observations are equivalent only when moving and merging them preserves
/// every property shared by CommonObservationMotion and
/// LoopInvariantObservationMotion.
inline bool isEquivalentMovableObservation(const Observation &LHS,
                                           const Observation &RHS) {
  return &LHS.getMemoryObject() == &RHS.getMemoryObject() &&
         LHS.isMergeable() && RHS.isMergeable() && LHS.isMovable() &&
         RHS.isMovable() && LHS.getLevel() == RHS.getLevel() &&
         LHS.getRegion() == RHS.getRegion();
}

} // namespace mair
