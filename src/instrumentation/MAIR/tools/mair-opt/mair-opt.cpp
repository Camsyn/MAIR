#include "MAIR/MAIR.h"
#include "MAIR/MAIRDump.h"
#include "MAIR/MAIRPasses.h"

#include "llvm/ADT/iterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace mair;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input LLVM IR>"), cl::init("-"),
                  cl::value_desc("filename"));
static cl::opt<bool>
    EmitLLVM("emit-llvm", cl::desc("Print LLVM IR annotated with MAIR"),
             cl::init(false));
static cl::opt<bool> DisableOutput("disable-output",
                                   cl::desc("Do not emit MAIR or LLVM output"),
                                   cl::init(false));
static cl::opt<std::string>
    PassPipeline("passes", cl::desc("MAIRGraph pass pipeline"),
                 cl::value_desc("pipeline"));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "MAIR optimizer utility\n");

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

  MAIRGraphAnalysisManager GAM;
  registerMAIRGraphAnalyses(GAM, &PIC);
  MAIRGraphPassManager GPM;
  if (!PassPipeline.empty()) {
    if (Error Err = parseMAIRGraphPassPipeline(GPM, PassPipeline)) {
      errs() << toString(std::move(Err)) << "\n";
      return 1;
    }
  }

  SmallVector<MAIRGraph, 8> Graphs;
  for (Function &F : *M) {
    if (F.isDeclaration())
      continue;

    Graphs.push_back(MAIRGraph::build<DefaultBarrierTrait>(F, FAM));
    MAIRGraph &Graph = Graphs.back();
    if (!Graph.verify(&errs())) {
      errs() << "invalid MAIRGraph for function " << F.getName() << "\n";
      Graph.print(errs());
      return 1;
    }

    if (!PassPipeline.empty()) {
      GPM.run(Graph, GAM);
      if (!Graph.verify(&errs())) {
        errs() << "invalid MAIRGraph after pass pipeline for function "
               << F.getName() << "\n";
        Graph.print(errs());
        return 1;
      }
      GAM.clear(Graph, Graph.getFunction() ? Graph.getFunction()->getName()
                                           : StringRef("<unknown>"));
    }

    if (!DisableOutput && !EmitLLVM) {
      dumpMAIR(Graph, outs());
    }
  }

  if (!DisableOutput && EmitLLVM && !Graphs.empty()) {
    SmallVector<const MAIRGraph *, 8> GraphPtrs(make_pointer_range(Graphs));
    dumpLLVM(GraphPtrs, outs());
  }

  return 0;
}
