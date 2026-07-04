#include "MAIR/MAIRDump.h"

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

namespace mair {

namespace {
class MAIRAnnotatedWriter final : public AssemblyAnnotationWriter {
  const MAIRGraph &Graph;
  void emitAnchoredObservations(const Instruction *I,
                                formatted_raw_ostream &OS) const;
  void emitObservationLine(const Observation &Obs,
                           formatted_raw_ostream &OS) const;

public:
  explicit MAIRAnnotatedWriter(const MAIRGraph &G) : Graph(G) {}

  void emitFunctionAnnot(const Function *F, formatted_raw_ostream &OS) override;
  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &OS) override;
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override;
};
} // namespace

void MAIRAnnotatedWriter::emitAnchoredObservations(
    const Instruction *I, formatted_raw_ostream &OS) const {
  for (const Observation &Obs : Graph)
    if (Obs.getAnchor() == I)
      emitObservationLine(Obs, OS);
}

void MAIRAnnotatedWriter::emitObservationLine(const Observation &Obs,
                                              formatted_raw_ostream &OS) const {
  OS << "; " << Obs.toString(true) << "\n";
}

void MAIRAnnotatedWriter::emitFunctionAnnot(const Function *F,
                                            formatted_raw_ostream &OS) {
  if (!Graph.getFunction() || Graph.getFunction() != F)
    return;
  OS << "; MAIR fn\n";
}

void MAIRAnnotatedWriter::emitInstructionAnnot(const Instruction *I,
                                               formatted_raw_ostream &OS) {
  emitAnchoredObservations(I, OS);
}

void MAIRAnnotatedWriter::printInfoComment(const Value &V,
                                           formatted_raw_ostream &OS) {
  (void)V;
  (void)OS;
}

void dumpPure(const MAIRGraph &Graph, raw_ostream &OS) { Graph.print(OS); }

void dumpAnnotated(const MAIRGraph &Graph, const Module &M, raw_ostream &OS) {
  MAIRAnnotatedWriter Writer(Graph);
  M.print(OS, &Writer);
}

} // namespace mair
