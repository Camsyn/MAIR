#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/IR/PassManager.h"

namespace mair {

/// Object bounds elimination (`boe`). Deletes every deletable observation
/// whose region provably lies within an object the sanitizer trusts
/// (MAIROptimizationConfig::getTrustedObjectSize): an access there is never
/// reported, so its check can never fire. Without a trusted object oracle, as
/// for every sanitizer by default, the pass changes nothing.
///
/// The region is proven with ScalarEvolution, then with LazyValueInfo where
/// the program point runs; docs/mair-object-bounds.md has the rules and the
/// e2e data.
class ObjectBoundsEliminationPass
    : public llvm::PassInfoMixin<ObjectBoundsEliminationPass> {
  unsigned Ordinal;

public:
  explicit ObjectBoundsEliminationPass(unsigned Ordinal = 0)
      : Ordinal(Ordinal) {}

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
