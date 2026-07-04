#include "MAIR/MAIRDump.h"

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

namespace mair {

namespace {
class MAIRLLVMWriter final : public AssemblyAnnotationWriter {
  const MAIRGraph &Graph;
  void emitAnchoredObservations(const Instruction *I,
                                formatted_raw_ostream &OS) const;
  void emitObservationLine(const Observation &Obs,
                           formatted_raw_ostream &OS) const;

public:
  explicit MAIRLLVMWriter(const MAIRGraph &G) : Graph(G) {}

  void emitFunctionAnnot(const Function *F, formatted_raw_ostream &OS) override;
  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &OS) override;
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override;
};
} // namespace

void MAIRLLVMWriter::emitAnchoredObservations(
    const Instruction *I, formatted_raw_ostream &OS) const {
  for (const Observation &Obs : Graph)
    if (Obs.getAnchor() == I)
      emitObservationLine(Obs, OS);
}

void MAIRLLVMWriter::emitObservationLine(const Observation &Obs,
                                         formatted_raw_ostream &OS) const {
  OS << "; " << Obs.toString(true) << "\n";
}

void MAIRLLVMWriter::emitFunctionAnnot(const Function *F,
                                       formatted_raw_ostream &OS) {
  if (!Graph.getFunction() || Graph.getFunction() != F)
    return;
  OS << "; MAIR fn\n";
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

void dumpLLVM(const MAIRGraph &Graph, const Module &M, raw_ostream &OS) {
  MAIRLLVMWriter Writer(Graph);
  M.print(OS, &Writer);
}

} // namespace mair
