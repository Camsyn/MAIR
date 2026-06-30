#include "MAIR/MAIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace __xsan::mair;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input LLVM IR>"), cl::Required);
static cl::opt<bool> Pure("pure", cl::desc("Print MAIR as standalone IR"),
                          cl::init(false));
static cl::opt<bool> Annotated("annotated",
                               cl::desc("Print LLVM IR annotated with MAIR"),
                               cl::init(false));

static void buildFunctionGraph(const Function &F, MAIRGraph &Graph) {
  unsigned Index = 0;
  llvm::DenseMap<const BasicBlock *, Observation *> EntryObs;
  for (const BasicBlock &BB : F) {
    auto &Obs = Graph.createObservation((Twine("bb") + Twine(Index++)).str(),
                                        &BB, nullptr,
                                        Observation::SiteKind::BasicBlockEntry);
    EntryObs[&BB] = &Obs;
  }

  for (const BasicBlock &BB : F) {
    Observation *From = EntryObs.lookup(&BB);
    if (BB.empty()) {
      errs() << "MAIRGraph builder found non-well-formed block ";
      if (BB.hasName())
        errs() << BB.getName();
      else
        errs() << "<unnamed>";
      errs() << " in function " << F.getName() << "\n";
      report_fatal_error("MAIRGraph builder expects well-formed basic blocks");
    }
    const Instruction *Term = &BB.back();
    for (unsigned I = 0, E = Term->getNumSuccessors(); I != E; ++I) {
      const BasicBlock *Succ = Term->getSuccessor(I);
      if (Observation *To = EntryObs.lookup(Succ))
        Graph.addEdge(*From, *To);
    }
  }

  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      std::string InstText;
      raw_string_ostream InstOS(InstText);
      I.print(InstOS);
      InstOS.flush();

      StringRef Text = StringRef(InstText).ltrim();
      if (!Text.contains(" load ") && !Text.starts_with("store "))
        continue;

      auto &Obs =
          Graph.createObservation((Twine("o") + Twine(Index++)).str(), &BB, &I,
                                  Observation::SiteKind::MemoryInst);
      Obs.addAccess(
          MemoryAccess::fromInstruction(I, F.getParent()->getDataLayout()));
      if (Observation *Entry = EntryObs.lookup(&BB))
        Graph.addEdge(*Entry, Obs);
    }
  }
}

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

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    MAIRGraph Graph(F);
    buildFunctionGraph(F, Graph);
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
