#pragma once

#include "MAIR/MAIRPasses.h"
#include "MAIR/Materialization/MAIRMaterialization.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/Error.h"

#include <cassert>
#include <cstdlib>
#include <memory>

namespace mair {

/// Whether sanitizer-side MAIR optimization was explicitly enabled.
inline bool isMAIROptEnabled() { return std::getenv("MAIR") != nullptr; }

/// Owns the complete MAIR lifecycle for one mutable LLVM function.
///
/// The barrier policy is a type because it is part of graph construction, not
/// a run-time optimization choice. Keeping the graph and its pass managers in
/// this object also makes the observation-to-access view valid until
/// instantiate() consumes the graph.
template <class BarrierTrait = DefaultBarrierTrait> class MAOpt {
public:
  using ObservationAccesses =
      llvm::DenseMap<const Observation *,
                     llvm::SmallVector<const MemoryAccess *, 4>>;

  MAOpt(llvm::Function &F, llvm::FunctionAnalysisManager &FAM,
        llvm::PassInstrumentationCallbacks *PIC, bool KeepFullDebug)
      : F(F), FAM(FAM), KeepFullDebug(KeepFullDebug) {
    registerMAIRGraphAnalyses(GAM, PIC, &FAM);
  }

  MAOpt(const MAOpt &) = delete;
  MAOpt &operator=(const MAOpt &) = delete;

  MAIRGraph &build() {
    assert(!Graph && "MAOpt graph has already been built");
    Graph = std::make_unique<MAIRGraph>(
        MAIRGraph::template build<BarrierTrait>(F, FAM));
    rebuildObservationAccesses();
    return *Graph;
  }

  llvm::Error optimize() {
    if (!Graph)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "MAOpt graph has not been built");
    if (Optimized)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "MAOpt graph has already been optimized");

    if (KeepFullDebug)
      buildMAIRGraphOgPipeline(PM);
    else
      buildMAIRGraphO1Pipeline(PM);
    PM.run(*Graph, GAM);
    if (!Graph->verify())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "MAOpt produced an invalid MAIRGraph");
    GAM.clear(*Graph, F.getName());
    Optimized = true;
    rebuildObservationAccesses();
    return llvm::Error::success();
  }

  llvm::Expected<MAIRInstantiationResult>
  instantiate(MAIRObservationBackend &Backend) {
    if (!Graph)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "MAOpt graph has not been built");
    if (!Optimized)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "MAOpt graph has not been optimized");
    llvm::Expected<MAIRInstantiationResult> Result =
        Graph->instantiate(FAM, Backend);
    if (Result)
      ObservationToAccesses.clear();
    return Result;
  }

  MAIRGraph &getGraph() {
    assert(Graph && "MAOpt graph has not been built");
    return *Graph;
  }
  const MAIRGraph &getGraph() const {
    assert(Graph && "MAOpt graph has not been built");
    return *Graph;
  }

  const ObservationAccesses &getObservationMAs() const {
    assert(Graph && "MAOpt graph has not been built");
    return ObservationToAccesses;
  }

  llvm::ArrayRef<const MemoryAccess *>
  getObservationMAs(const Observation &Obs) const {
    assert(Graph && "MAOpt graph has not been built");
    const auto It = ObservationToAccesses.find(&Obs);
    if (It == ObservationToAccesses.end())
      return {};
    return It->second;
  }

private:
  void rebuildObservationAccesses() {
    ObservationToAccesses.clear();
    for (const Observation &Obs : *Graph) {
      auto &Accesses = ObservationToAccesses[&Obs];
      for (const MemoryAccess &Access : Obs)
        Accesses.push_back(&Access);
    }
  }

  llvm::Function &F;
  llvm::FunctionAnalysisManager &FAM;
  bool KeepFullDebug;
  bool Optimized = false;
  std::unique_ptr<MAIRGraph> Graph;
  MAIRGraphAnalysisManager GAM;
  MAIRGraphPassManager PM;
  ObservationAccesses ObservationToAccesses;
};

} // namespace mair
