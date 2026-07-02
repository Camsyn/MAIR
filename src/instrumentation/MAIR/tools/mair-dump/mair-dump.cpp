#include "MAIR/MAIR.h"
#include "MAIR/MAIRDump.h"

#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace mair;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input LLVM IR>"), cl::Required);
static cl::opt<bool> Pure("pure", cl::desc("Print MAIR as standalone IR"),
                          cl::init(false));
static cl::opt<bool> Annotated("annotated",
                               cl::desc("Print LLVM IR annotated with MAIR"),
                               cl::init(false));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "MAIR dump utility\n");

  if (!Pure && !Annotated) {
    Pure = true;
    Annotated = true;
  }

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  FAM.registerPass([&] { return PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return DominatorTreeAnalysis(); });
  FAM.registerPass([] { return PostDominatorTreeAnalysis(); });

  for (Function &F : *M) {
    if (F.isDeclaration())
      continue;

    MAIRGraph Graph = MAIRGraph::build<DefaultBarrierTrait>(F, FAM);
    if (!Graph.verify()) {
      errs() << "invalid MAIRGraph for function " << F.getName() << "\n";
      Graph.print(errs());
      return 1;
    }

    if (Pure)
      dumpPure(Graph, outs());
    if (Annotated)
      dumpAnnotated(Graph, *M, outs());
  }

  return 0;
}
