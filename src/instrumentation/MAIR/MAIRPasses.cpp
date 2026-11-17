#include "MAIR/MAIRPasses.h"
#include "MAIR/MAIRStatLog.h"

#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/Transforms/CommonObservationMotionPass.h"
#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIR/Transforms/ObjectBoundsEliminationPass.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManagerImpl.h"
#include "llvm/Support/CommandLine.h"
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

cl::opt<std::string> ClMAIRPipeline(
    "mair-pipeline", cl::Hidden,
    cl::desc("Replace the MAIR pipeline that MAOpt runs (for tuning); same "
             "syntax as mair-opt -passes"));

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

/// Run \p Passes on each graph of the tree in turn, children first.
template <typename... PassTs>
void addHierarchyPass(MAIRGraphPassManager &PM, PassTs... Passes) {
  MAIRGraphPassManager LocalPM;
  (LocalPM.addPass(std::move(Passes)), ...);
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
                               PassInstrumentationCallbacks *PIC,
                               FunctionAnalysisManager *FAM) {
  AM.registerPass([PIC] { return PassInstrumentationAnalysis(PIC); });
  AM.registerPass([FAM] { return LoopAnalysis(FAM); });
#define MAIR_GRAPH_ANALYSIS(NAME, CREATE_PASS)                                 \
  AM.registerPass([] { return CREATE_PASS; });
#include "MAIR/PassRegistry.def"
}

void buildMAIRGraphOgPipeline(MAIRGraphPassManager &PM,
                              bool EliminateWithinTrustedObjects) {
  noteMAIRPipeline("Og");
  addHierarchyPass(PM, SimplifyGraphPass(/*Ordinal=*/1));
  addHierarchyPass(PM, LoopInvariantObservationMotionPass(
                           /*MergeEquivalentObservations=*/false,
                           /*Ordinal=*/1));
  addHierarchyPass(PM, LoopPeriodicObservationCombinePass(
                           /*MergeEquivalentObservations=*/false));
  // Inner combination exports into the parent after that parent has already
  // moved its original invariants. Run motion again so the parent can hoist
  // the synthesized observation.
  addHierarchyPass(PM, LoopInvariantObservationMotionPass(
                           /*MergeEquivalentObservations=*/false,
                           /*Ordinal=*/2));
  addHierarchyPass(PM, RedundantObservationEliminationPass(/*Ordinal=*/1));
  if (EliminateWithinTrustedObjects)
    addHierarchyPass(PM, ObjectBoundsEliminationPass());
  addHierarchyPass(PM, SimplifyGraphPass(/*Ordinal=*/2));
}

void buildMAIRGraphO1Pipeline(MAIRGraphPassManager &PM,
                              bool EliminateWithinTrustedObjects) {
  // Normalize topology, then eliminate redundant observations before motion
  // (so that sinking keeps one source guard). Location merging runs before
  // loop motion and periodic combination: accesses merged in a loop body,
  // such as p[2i] and p[2i+1], become one access whose step equals its size,
  // which periodic combination summarizes as one contiguous range instead of
  // two strided periodic regions. The three run on each loop before its
  // parent, so the parent merges, hoists and combines again what its inner
  // loops have just summarized, such as the rows of a matrix. A second
  // invariant motion and location merge after the group handled those
  // summaries before the grouping; they changed nothing on any e2e project
  // afterwards and are gone. The second elimination removes accesses the
  // merged regions cover. Object
  // bounds elimination, when the sanitizer trusts some objects, runs last: an
  // observation it deletes can no longer cover another one for the redundancy
  // elimination before it. The final simplification removes program points
  // emptied by those transforms.
  // docs/mair-loops.md has the e2e data behind this order.
  noteMAIRPipeline("O1");
  addHierarchyPass(PM, SimplifyGraphPass(/*Ordinal=*/1));
  addHierarchyPass(PM, RedundantObservationEliminationPass(/*Ordinal=*/1));
  addHierarchyPass(PM, LocationMergePass(/*Ordinal=*/1),
                   LoopInvariantObservationMotionPass(
                       /*MergeEquivalentObservations=*/true, /*Ordinal=*/1),
                   LoopPeriodicObservationCombinePass());
  addHierarchyPass(PM, RedundantObservationEliminationPass(/*Ordinal=*/2));
  if (EliminateWithinTrustedObjects)
    addHierarchyPass(PM, ObjectBoundsEliminationPass());
  addHierarchyPass(PM, SimplifyGraphPass(/*Ordinal=*/2));
}

void buildMAIRGraphDefaultPipeline(MAIRGraphPassManager &PM, bool KeepFullDebug,
                                   bool EliminateWithinTrustedObjects) {
  if (!ClMAIRPipeline.empty()) {
    noteMAIRPipeline("custom");
    if (Error Err = parseMAIRGraphPassPipeline(PM, ClMAIRPipeline))
      report_fatal_error(Twine("-mair-pipeline: ") + toString(std::move(Err)));
    return;
  }
  if (KeepFullDebug)
    buildMAIRGraphOgPipeline(PM, EliminateWithinTrustedObjects);
  else
    buildMAIRGraphO1Pipeline(PM, EliminateWithinTrustedObjects);
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

    // `NAME#N` numbers a repeated pass in `-mair-stats`, as the default
    // pipelines do.
    unsigned Ordinal = 0;
    auto [Name, OrdinalText] = PassName.split('#');
    if (!OrdinalText.empty()) {
      if (OrdinalText.getAsInteger(10, Ordinal) || Ordinal == 0)
        return createStringError("bad ordinal in MAIRGraph pass '" + PassName +
                                 "'");
      PassName = Name;
    }

#define MAIR_GRAPH_PASS(NAME, CREATE_PASS)                                     \
  if (PassName == NAME) {                                                      \
    addHierarchyPass(PM, CREATE_PASS);                                         \
    continue;                                                                  \
  }
#include "MAIR/PassRegistry.def"

    return createStringError("unknown MAIRGraph pass '" + PassName + "'");
  }
  return Error::success();
}

} // namespace mair
