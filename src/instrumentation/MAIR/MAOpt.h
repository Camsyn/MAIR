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

/// Whether sanitizer-side MAIR optimization was explicitly enabled.
inline bool isMAIROptEnabled() { return std::getenv("MAIR") != nullptr; }

inline llvm::SmallPtrSet<const llvm::Instruction *, 32>
collectModeledSources(const MAIRGraph &Graph) {
  llvm::SmallPtrSet<const llvm::Instruction *, 32> Sources;
  for (const ProgramPoint *Point : Graph.allProgramPoints())
    for (const Observation &Obs : *Point)
      Sources.insert_range(Obs.getInstructions());
  return Sources;
}

struct MAIRObservationSelection {
  llvm::SmallPtrSet<const Observation *, 32> Targets;
  llvm::SmallPtrSet<const llvm::Instruction *, 32> HandledSources;
};

inline MAIRObservationSelection selectMaterializableObservations(
    const MAIRGraph &Graph,
    const llvm::SmallPtrSetImpl<const llvm::Instruction *> &ModeledSources,
    llvm::function_ref<bool(const Observation &)> CanInstrument) {
  llvm::SmallVector<const Observation *, 32> Candidates;
  llvm::SmallPtrSet<const llvm::Instruction *, 32> NativeSources;
  for (const ProgramPoint *Point : Graph.allProgramPoints()) {
    for (const Observation &Obs : *Point) {
      if (Obs.isCallLike())
        continue;
      if (CanInstrument(Obs)) {
        assert(Obs.getInstructions().size() == 1 &&
               "materializable observations must have one source");
        Candidates.push_back(&Obs);
      } else {
        NativeSources.insert_range(Obs.getInstructions());
      }
    }
  }
  MAIRObservationSelection Result;
  for (const Observation *Obs : Candidates)
    if (!NativeSources.contains(Obs->getInstructions().front()))
      Result.Targets.insert(Obs);
  Result.HandledSources.insert_range(ModeledSources);
  for (const llvm::Instruction *Source : NativeSources)
    Result.HandledSources.erase(Source);
  return Result;
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
    for (MAIRGraph *Scope : Graph->graphs())
      GAM.clear(*Scope, F.getName());
    Optimized = true;
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
