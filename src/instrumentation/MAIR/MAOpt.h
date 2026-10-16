#pragma once

#include "MAIR/MAIRPasses.h"
#include "MAIR/Materialization/MAIRMaterialization.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
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
/// this object also makes the program-point-to-observation view valid until
/// instantiate() consumes the graph.
template <class BarrierTrait = DefaultBarrierTrait> class MAOpt {
public:
  using ProgramPointObservations =
      llvm::DenseMap<const ProgramPoint *,
                     llvm::SmallVector<const Observation *, 4>>;

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
    Graph = std::make_unique<MAIRGraph>(
        MAIRGraph::template build<BarrierTrait>(F, FAM, Config));
    rebuildProgramPointObservations();
    return *Graph;
  }

  void retainObservations(
      const llvm::SmallPtrSetImpl<const Observation *> &Targets) {
    assert(Graph && "MAOpt graph has not been built");

    for (ProgramPoint &PP : *Graph) {
      for (auto It = PP.begin(); It != PP.end();) {
        if (Targets.contains(&*It))
          ++It;
        else
          It = PP.erase(It);
      }
    }
    rebuildProgramPointObservations();
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
    rebuildProgramPointObservations();
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
      ProgramPointToObservations.clear();
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

  const ProgramPointObservations &getProgramPointObservations() const {
    assert(Graph && "MAOpt graph has not been built");
    return ProgramPointToObservations;
  }

  llvm::ArrayRef<const Observation *>
  getProgramPointObservations(const ProgramPoint &PP) const {
    assert(Graph && "MAOpt graph has not been built");
    const auto It = ProgramPointToObservations.find(&PP);
    if (It == ProgramPointToObservations.end())
      return {};
    return It->second;
  }

private:
  void rebuildProgramPointObservations() {
    ProgramPointToObservations.clear();
    for (const ProgramPoint &PP : *Graph) {
      auto &Observations = ProgramPointToObservations[&PP];
      for (const Observation &Obs : PP)
        Observations.push_back(&Obs);
    }
  }

  llvm::Function &F;
  llvm::FunctionAnalysisManager &FAM;
  bool KeepFullDebug;
  MAIROptimizationConfig Config;
  bool Optimized = false;
  std::unique_ptr<MAIRGraph> Graph;
  MAIRGraphAnalysisManager GAM;
  MAIRGraphPassManager PM;
  ProgramPointObservations ProgramPointToObservations;
};

} // namespace mair
