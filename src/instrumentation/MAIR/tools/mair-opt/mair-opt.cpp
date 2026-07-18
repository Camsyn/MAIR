#include "MAIR/MAIR.h"
#include "MAIR/MAIRDump.h"
#include "MAIR/MAIRPasses.h"

#include "llvm/ADT/iterator.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace mair;

static cl::OptionCategory MAIROptions("MAIR Options");

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input LLVM IR>"), cl::init("-"),
                  cl::value_desc("filename"), cl::cat(MAIROptions));
static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Write output to <file>"),
                                           cl::value_desc("filename"),
                                           cl::init("-"), cl::cat(MAIROptions));
static cl::opt<bool> EmitLLVM("emit-llvm",
                              cl::desc("Print LLVM IR annotated with MAIR"),
                              cl::init(false), cl::cat(MAIROptions));
static cl::opt<bool> EmitDOT("emit-dot",
                             cl::desc("Print MAIR graph as Graphviz DOT"),
                             cl::init(false), cl::cat(MAIROptions));
static cl::opt<bool> DisableOutput("disable-output",
                                   cl::desc("Do not emit MAIR or LLVM output"),
                                   cl::init(false), cl::cat(MAIROptions));
static cl::opt<bool>
    Og("Og", cl::desc("Run the standard MAIR Og optimization pipeline"),
       cl::init(false), cl::cat(MAIROptions));
static cl::opt<bool>
    O1("O1", cl::desc("Run the standard MAIR O1 optimization pipeline"),
       cl::init(false), cl::cat(MAIROptions));
static cl::opt<std::string> PassPipeline("passes",
                                         cl::desc("MAIRGraph pass pipeline"),
                                         cl::value_desc("pipeline"),
                                         cl::cat(MAIROptions));
static cl::extrahelp MAIRGraphPassHelp(R"(
MAIRGraph pass names for -passes:
  cmm                           - Common MA Motion
  liom                          - Loop Invariant Observation Motion
  lpoc                          - Loop Periodic Observation Combine
  locationmerge                 - Location Merge
  roe                           - Redundant Observation Elimination
  simplifygraph                 - Simplify Graph
)");

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions(MAIROptions);
  cl::ParseCommandLineOptions(argc, argv, "MAIR optimizer utility\n");
  if (EmitLLVM && EmitDOT) {
    errs() << "-emit-llvm and -emit-dot cannot be used together\n";
    return 1;
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
  FAM.registerPass([] { return TargetLibraryAnalysis(); });
  FAM.registerPass([] { return TargetIRAnalysis(); });
  FAM.registerPass([] { return AssumptionAnalysis(); });
  FAM.registerPass([] { return DominatorTreeAnalysis(); });
  FAM.registerPass([] { return PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  FAM.registerPass([] { return ScalarEvolutionAnalysis(); });

  MAIRGraphAnalysisManager GAM;
  registerMAIRGraphAnalyses(GAM, &PIC, &FAM);
  MAIRGraphPassManager GPM;
  if ((Og || O1) && !PassPipeline.empty()) {
    errs() << "-Og/-O1 cannot be combined with -passes\n";
    return 1;
  }
  if (Og && O1) {
    errs() << "-Og cannot be combined with -O1\n";
    return 1;
  }
  if (Og) {
    buildMAIRGraphOgPipeline(GPM);
  } else if (O1) {
    buildMAIRGraphO1Pipeline(GPM);
  } else if (!PassPipeline.empty()) {
    if (Error Err = parseMAIRGraphPassPipeline(GPM, PassPipeline)) {
      errs() << toString(std::move(Err)) << "\n";
      return 1;
    }
  }

  if (EmitDOT) {
    unsigned NumDefinitions = 0;
    for (const Function &F : *M)
      if (!F.isDeclaration())
        ++NumDefinitions;
    if (NumDefinitions != 1) {
      errs() << "-emit-dot expects exactly one function definition; found "
             << NumDefinitions << "\n";
      return 1;
    }
  }

  std::unique_ptr<ToolOutputFile> OutputFile;
  raw_ostream *OutputOS = &outs();
  if (!DisableOutput) {
    std::error_code EC;
    OutputFile =
        std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_Text);
    if (EC) {
      errs() << "failed to open output file '" << OutputFilename
             << "': " << EC.message() << "\n";
      return 1;
    }
    OutputOS = &OutputFile->os();
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

    if (Og || O1 || !PassPipeline.empty()) {
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

    if (!DisableOutput && EmitDOT) {
      dumpDOT(Graph, *OutputOS);
    } else if (!DisableOutput && !EmitLLVM) {
      dumpMAIR(Graph, *OutputOS);
    }
  }

  if (!DisableOutput && EmitLLVM && !Graphs.empty()) {
    SmallVector<const MAIRGraph *, 8> GraphPtrs(make_pointer_range(Graphs));
    dumpLLVM(GraphPtrs, *OutputOS);
  }

  if (OutputFile)
    OutputFile->keep();

  return 0;
}
