#pragma once

#include "MAIR/MAIR.h"

namespace mair {

/// Accesses are equivalent only when moving and merging them preserves every
/// property shared by CommonMAMotion and LoopInvariantObservationMotion.
inline bool isEquivalentMovableAccess(const MemoryAccess &LHS,
                                      const MemoryAccess &RHS) {
  return &LHS.getMemoryObject() == &RHS.getMemoryObject() &&
         LHS.isMergeable() && RHS.isMergeable() && LHS.isMovable() &&
         RHS.isMovable() && LHS.getLevel() == RHS.getLevel() &&
         LHS.getRegion() == RHS.getRegion();
}

} // namespace mair
