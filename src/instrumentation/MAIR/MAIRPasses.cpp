#include "MAIR/MAIRPasses.h"

#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/Transforms/CommonObservationMotionPass.h"
#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManagerImpl.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

using namespace llvm;

namespace llvm {
template <>
void printIRUnitNameForStackTrace<mair::MAIRGraph>(raw_ostream &OS,
                                                   const mair::MAIRGraph &G) {
  if (const Function *F = G.getFunction()) {
    OS << "MAIRGraph for function " << F->getName();
    return;
  }
  OS << "MAIRGraph";
}

template class AnalysisManager<mair::MAIRGraph>;
template class PassManager<mair::MAIRGraph>;
} // namespace llvm

namespace mair {

namespace {

class MAIRGraphHierarchyPass : public PassInfoMixin<MAIRGraphHierarchyPass> {
  MAIRGraphPassManager PM;

public:
  explicit MAIRGraphHierarchyPass(MAIRGraphPassManager PM)
      : PM(std::move(PM)) {}

  PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM) {
    bool Changed = false;
    for (const auto &Child : G.children())
      Changed |= !run(*Child, AM).areAllPreserved();
    if (Changed)
      AM.invalidate(G, PreservedAnalyses::none());
    Changed |= !PM.run(G, AM).areAllPreserved();
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

template <typename PassT>
void addHierarchyPass(MAIRGraphPassManager &PM, PassT Pass) {
  MAIRGraphPassManager LocalPM;
  LocalPM.addPass(std::move(Pass));
  PM.addPass(MAIRGraphHierarchyPass(std::move(LocalPM)));
}

} // namespace

bool normalizeMAIRLoops(Function &F, FunctionAnalysisManager &FAM) {
  FAM.registerPass([] { return ScalarEvolutionAnalysis(); });
  FAM.registerPass([] { return MemorySSAAnalysis(); });
  PreservedAnalyses Preserved = LoopSimplifyPass().run(F, FAM);
  bool Changed = !Preserved.areAllPreserved();
  FAM.invalidate(F, Preserved);
  return Changed;
}

void registerMAIRGraphAnalyses(MAIRGraphAnalysisManager &AM,
                               PassInstrumentationCallbacks *PIC) {
  registerMAIRGraphAnalyses(AM, PIC, nullptr);
}

void registerMAIRGraphAnalyses(MAIRGraphAnalysisManager &AM,
                               PassInstrumentationCallbacks *PIC,
                               FunctionAnalysisManager *FAM) {
  AM.registerPass([PIC] { return PassInstrumentationAnalysis(PIC); });
  AM.registerPass([FAM] { return LoopAnalysis(FAM); });
#define MAIR_GRAPH_ANALYSIS(NAME, CREATE_PASS)                                 \
  AM.registerPass([] { return CREATE_PASS; });
#include "MAIR/PassRegistry.def"
}

void buildMAIRGraphOgPipeline(MAIRGraphPassManager &PM) {
  addHierarchyPass(PM, SimplifyGraphPass());
  addHierarchyPass(PM, LoopInvariantObservationMotionPass(
                           /*MergeEquivalentObservations=*/false));
  addHierarchyPass(PM, LoopPeriodicObservationCombinePass(
                           /*MergeEquivalentObservations=*/false));
  addHierarchyPass(PM, RedundantObservationEliminationPass());
  addHierarchyPass(PM, SimplifyGraphPass());
}

void buildMAIRGraphO1Pipeline(MAIRGraphPassManager &PM) {
  // Normalize topology before motion, combine loop and location opportunities,
  // then alternate global motion with local merging/elimination. The final
  // simplification removes program points emptied by those observation
  // transforms.
  addHierarchyPass(PM, SimplifyGraphPass());
  addHierarchyPass(PM, LoopInvariantObservationMotionPass());
  addHierarchyPass(PM, LoopPeriodicObservationCombinePass());
  addHierarchyPass(PM, LocationMergePass());
  addHierarchyPass(PM, RedundantObservationEliminationPass());
  addHierarchyPass(PM, CommonObservationMotionPass());
  addHierarchyPass(PM, LocationMergePass());
  addHierarchyPass(PM, RedundantObservationEliminationPass());
  addHierarchyPass(PM, SimplifyGraphPass());
}

Error parseMAIRGraphPassPipeline(MAIRGraphPassManager &PM,
                                 StringRef PipelineText) {
  SmallVector<StringRef, 8> Pipeline;
  PipelineText.split(Pipeline, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (StringRef PassName : Pipeline) {
    PassName = PassName.trim();
    if (PassName.empty())
      continue;

    if (PassName == "default<Og>") {
      buildMAIRGraphOgPipeline(PM);
      continue;
    }
    if (PassName == "default<O1>") {
      buildMAIRGraphO1Pipeline(PM);
      continue;
    }
    if (PassName == "print<stat>") {
      PM.addPass(MAIRStatPrinterPass(errs()));
      continue;
    }

#define MAIR_GRAPH_PASS(NAME, CREATE_PASS)                                     \
  if (PassName == NAME) {                                                      \
    addHierarchyPass(PM, CREATE_PASS);                                         \
    continue;                                                                  \
  }
#include "MAIR/PassRegistry.def"

    return createStringError(inconvertibleErrorCode(),
                             "unknown MAIRGraph pass '%s'",
                             PassName.str().c_str());
  }
  return Error::success();
}

} // namespace mair
