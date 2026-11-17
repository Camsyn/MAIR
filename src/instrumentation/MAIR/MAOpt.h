#pragma once

#include "MAIR/MAIRPasses.h"
#include "MAIR/Materialization/MAIRMaterialization.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/Error.h"

#include <cassert>
#include <cstdlib>
#include <memory>

namespace mair {

namespace detail {
inline thread_local bool MAIROptSuppressed = false;
} // namespace detail

/// Whether sanitizer-side MAIR optimization was explicitly enabled, and is not
/// turned off for the pipeline being run (MAIROptSuppressionScope).
inline bool isMAIROptEnabled() {
  return !detail::MAIROptSuppressed && std::getenv("MAIR") != nullptr;
}

/// Turns sanitizer-side MAIR optimization off while it lives. Clang's -O0
/// pipeline runs the sanitizers in one (PassRegistry.cpp), so that their
/// output is the native one.
class MAIROptSuppressionScope {
  bool WasSuppressed = detail::MAIROptSuppressed;

public:
  MAIROptSuppressionScope() { detail::MAIROptSuppressed = true; }
  ~MAIROptSuppressionScope() { detail::MAIROptSuppressed = WasSuppressed; }
  MAIROptSuppressionScope(const MAIROptSuppressionScope &) = delete;
  MAIROptSuppressionScope &operator=(const MAIROptSuppressionScope &) = delete;
};

inline llvm::SmallPtrSet<const llvm::Instruction *, 32>
collectModeledSources(const MAIRGraph &Graph) {
  llvm::SmallPtrSet<const llvm::Instruction *, 32> Sources;
  for (const ProgramPoint *Point : Graph.allProgramPoints())
    for (const Observation &Obs : *Point)
      Sources.insert_range(Obs.getInstructions());
  return Sources;
}

/// Owns the complete MAIR lifecycle for one mutable LLVM function.
///
/// The barrier policy is a type because it is part of graph construction, not
/// a run-time optimization choice.
template <class BarrierTrait = DefaultBarrierTrait> class MAOpt {
public:
  MAOpt(llvm::Function &F, llvm::FunctionAnalysisManager &FAM,
        llvm::PassInstrumentationCallbacks *PIC, bool KeepFullDebug,
        MAIROptimizationConfig Config = MAIROptimizationConfig::generic())
      : F(F), FAM(FAM), KeepFullDebug(KeepFullDebug), Config(Config) {
    registerMAIRGraphAnalyses(GAM, PIC, &FAM);
  }

  MAOpt(const MAOpt &) = delete;
  MAOpt &operator=(const MAOpt &) = delete;

  MAIRGraph &build() {
    assert(!Graph && "MAOpt graph has already been built");
    ModifiedIR = normalizeMAIRLoops(F, FAM);
    Graph = std::make_unique<MAIRGraph>(
        MAIRGraph::template build<BarrierTrait>(F, FAM, Config));
    return *Graph;
  }

  void retainObservations(
      const llvm::SmallPtrSetImpl<const Observation *> &Targets) {
    assert(Graph && "MAOpt graph has not been built");

    for (ProgramPoint *Point : Graph->allProgramPoints()) {
      ProgramPoint &PP = *Point;
      for (auto It = PP.begin(); It != PP.end();) {
        if (Targets.contains(&*It))
          ++It;
        else
          It = PP.erase(It);
      }
    }
  }

  /// Drop the observations \p Unwanted selects. Before optimization, a
  /// sanitizer drops those of the accesses its native lowering never
  /// instruments. Such an observation would otherwise merge with others and
  /// make the merged one fall back to native lowering as a whole. Dropping it
  /// removes no check from another access: an observation only ever covers or
  /// merges with others, so without it they keep their own checks. Its source
  /// is then not modeled, and native lowering skips it as before.
  void
  eraseObservationsIf(llvm::function_ref<bool(const Observation &)> Unwanted) {
    assert(Graph && "MAOpt graph has not been built");
    for (ProgramPoint *Point : Graph->allProgramPoints()) {
      ProgramPoint &PP = *Point;
      for (auto It = PP.begin(); It != PP.end();)
        It = Unwanted(*It) ? PP.erase(It) : std::next(It);
    }
  }

  llvm::Error optimize() {
    if (!Graph)
      return llvm::createStringError("MAOpt graph has not been built");
    if (Optimized)
      return llvm::createStringError("MAOpt graph has already been optimized");

    buildMAIRGraphDefaultPipeline(PM, KeepFullDebug);
    PM.run(*Graph, GAM);
#ifndef NDEBUG
    // Verification walks every program point, edge and observation again, and
    // instantiate() verifies the graph before it plans the lowering anyway.
    // So only assertion-enabled builds also check right after the pipeline,
    // as mair-opt does.
    if (!Graph->verify())
      return llvm::createStringError("MAOpt produced an invalid MAIRGraph");
#endif
    for (MAIRGraph *Scope : Graph->graphs())
      GAM.clear(*Scope, F.getName());
    Optimized = true;
    return llvm::Error::success();
  }

  llvm::Expected<MAIRInstantiationResult>
  instantiate(MAIRObservationBackend &Backend) {
    if (!Graph)
      return llvm::createStringError("MAOpt graph has not been built");
    if (!Optimized)
      return llvm::createStringError("MAOpt graph has not been optimized");
    return Graph->instantiate(FAM, Backend);
  }

  MAIRGraph &getGraph() {
    assert(Graph && "MAOpt graph has not been built");
    return *Graph;
  }
  const MAIRGraph &getGraph() const {
    assert(Graph && "MAOpt graph has not been built");
    return *Graph;
  }

  bool hasModifiedIR() const { return ModifiedIR; }

private:
  llvm::Function &F;
  llvm::FunctionAnalysisManager &FAM;
  bool KeepFullDebug;
  MAIROptimizationConfig Config;
  bool Optimized = false;
  bool ModifiedIR = false;
  std::unique_ptr<MAIRGraph> Graph;
  MAIRGraphAnalysisManager GAM;
  MAIRGraphPassManager PM;
};

} // namespace mair
