#include "MAIR/MAIRPasses.h"

#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManagerImpl.h"

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

void registerMAIRGraphAnalyses(MAIRGraphAnalysisManager &AM,
                               PassInstrumentationCallbacks *PIC) {
  AM.registerPass([PIC] { return PassInstrumentationAnalysis(PIC); });
#define MAIR_GRAPH_ANALYSIS(NAME, CREATE_PASS)                                \
  AM.registerPass([] { return CREATE_PASS; });
#include "MAIR/PassRegistry.def"
}

Error parseMAIRGraphPassPipeline(MAIRGraphPassManager &PM,
                                 StringRef PipelineText) {
  SmallVector<StringRef, 8> Pipeline;
  PipelineText.split(Pipeline, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (StringRef PassName : Pipeline) {
    PassName = PassName.trim();
    if (PassName.empty())
      continue;

#define MAIR_GRAPH_PASS(NAME, CREATE_PASS)                                     \
  if (PassName == NAME) {                                                      \
    PM.addPass(CREATE_PASS);                                                   \
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
