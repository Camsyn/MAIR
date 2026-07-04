#include "MAIR/MAIRDump.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace mair {

namespace {
class MAIRLLVMWriter final : public AssemblyAnnotationWriter {
  ArrayRef<const MAIRGraph *> Graphs;
  void emitAnchoredObservations(const Instruction *I,
                                formatted_raw_ostream &OS) const;
  void emitObservationLine(const Observation &Obs,
                           formatted_raw_ostream &OS) const;
  const MAIRGraph *findGraph(const Function *F) const;

public:
  explicit MAIRLLVMWriter(ArrayRef<const MAIRGraph *> Gs) : Graphs(Gs) {}

  void emitFunctionAnnot(const Function *F, formatted_raw_ostream &OS) override;
  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &OS) override;
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override;
};
} // namespace

const MAIRGraph *MAIRLLVMWriter::findGraph(const Function *F) const {
  for (const MAIRGraph *Graph : Graphs)
    if (Graph && Graph->getFunction() == F)
      return Graph;
  return nullptr;
}

static const Function &getFunctionForDump(const MAIRGraph &Graph) {
  const Function *F = Graph.getFunction();
  if (!F)
    report_fatal_error("cannot dump LLVM for a MAIRGraph without a function");
  return *F;
}

static const Module &getModuleForDump(ArrayRef<const MAIRGraph *> Graphs) {
  if (Graphs.empty())
    report_fatal_error("cannot dump LLVM module without MAIR graphs");

  if (!Graphs.front())
    report_fatal_error("cannot dump LLVM module with a null MAIR graph");
  const Function &F = getFunctionForDump(*Graphs.front());
  const Module *M = F.getParent();
  if (!M)
    report_fatal_error("cannot dump LLVM for a function without a module");
  return *M;
}

void MAIRLLVMWriter::emitAnchoredObservations(
    const Instruction *I, formatted_raw_ostream &OS) const {
  const MAIRGraph *Graph = findGraph(I->getFunction());
  if (!Graph)
    return;
  for (const Observation &Obs : *Graph)
    if (Obs.getAnchor() == I)
      emitObservationLine(Obs, OS);
}

void MAIRLLVMWriter::emitObservationLine(const Observation &Obs,
                                         formatted_raw_ostream &OS) const {
  std::string Text = Obs.toString(true);
  SmallVector<StringRef, 4> Lines;
  StringRef(Text).split(Lines, '\n');
  for (StringRef Line : Lines)
    if (!Line.trim().empty())
      OS << "  ; " << Line << "\n";
}

void MAIRLLVMWriter::emitFunctionAnnot(const Function *F,
                                       formatted_raw_ostream &OS) {
  (void)F;
  (void)OS;
}

void MAIRLLVMWriter::emitInstructionAnnot(const Instruction *I,
                                          formatted_raw_ostream &OS) {
  emitAnchoredObservations(I, OS);
}

void MAIRLLVMWriter::printInfoComment(const Value &V,
                                      formatted_raw_ostream &OS) {
  (void)V;
  (void)OS;
}

void dumpMAIR(const MAIRGraph &Graph, raw_ostream &OS) { Graph.print(OS); }

void dumpLLVM(const MAIRGraph &Graph, raw_ostream &OS) {
  const MAIRGraph *Graphs[] = {&Graph};
  MAIRLLVMWriter Writer(Graphs);
  getFunctionForDump(Graph).print(OS, &Writer);
}

void dumpLLVM(ArrayRef<const MAIRGraph *> Graphs, raw_ostream &OS) {
  const Module &M = getModuleForDump(Graphs);
  MAIRLLVMWriter Writer(Graphs);
  M.print(OS, &Writer);
}

} // namespace mair
