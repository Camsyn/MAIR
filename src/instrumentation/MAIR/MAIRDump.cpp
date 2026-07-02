#include "MAIR/MAIRDump.h"

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

namespace mair {

namespace {
class MAIRAnnotatedWriter final : public AssemblyAnnotationWriter {
  const MAIRGraph &Graph;
  const Observation *findBlockObservation(const BasicBlock *BB,
                                          Observation::SiteKind Kind) const;
  const Observation *findInstructionObservation(const Instruction *I) const;
  void emitObservationLine(const Observation &Obs,
                           formatted_raw_ostream &OS) const;

public:
  explicit MAIRAnnotatedWriter(const MAIRGraph &G) : Graph(G) {}

  void emitFunctionAnnot(const Function *F, formatted_raw_ostream &OS) override;
  void emitBasicBlockStartAnnot(const BasicBlock *BB,
                                formatted_raw_ostream &OS) override;
  void emitBasicBlockEndAnnot(const BasicBlock *BB,
                              formatted_raw_ostream &OS) override;
  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &OS) override;
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override;
};
} // namespace

const Observation *
MAIRAnnotatedWriter::findBlockObservation(const BasicBlock *BB,
                                          Observation::SiteKind Kind) const {
  for (const Observation &Obs : Graph)
    if (Obs.getBasicBlock() == BB && Obs.getSiteKind() == Kind)
      return &Obs;
  return nullptr;
}

const Observation *
MAIRAnnotatedWriter::findInstructionObservation(const Instruction *I) const {
  for (const Observation &Obs : Graph)
    if (Obs.getAnchorInstruction() == I)
      return &Obs;
  return nullptr;
}

void MAIRAnnotatedWriter::emitObservationLine(const Observation &Obs,
                                              formatted_raw_ostream &OS) const {
  OS << "\n  ; " << Obs.toString(true);
}

void MAIRAnnotatedWriter::emitFunctionAnnot(const Function *F,
                                            formatted_raw_ostream &OS) {
  if (!Graph.getFunction() || Graph.getFunction() != F)
    return;
  OS << " ; MAIR fn";
}

void MAIRAnnotatedWriter::emitBasicBlockStartAnnot(const BasicBlock *BB,
                                                   formatted_raw_ostream &OS) {
  if (const Observation *Obs =
          findBlockObservation(BB, Observation::SiteKind::BasicBlockEntry))
    emitObservationLine(*Obs, OS);
}

void MAIRAnnotatedWriter::emitBasicBlockEndAnnot(const BasicBlock *BB,
                                                 formatted_raw_ostream &OS) {
  if (const Observation *Obs =
          findBlockObservation(BB, Observation::SiteKind::BasicBlockExit))
    emitObservationLine(*Obs, OS);
}

void MAIRAnnotatedWriter::emitInstructionAnnot(const Instruction *I,
                                               formatted_raw_ostream &OS) {
  if (const Observation *Obs = findInstructionObservation(I))
    emitObservationLine(*Obs, OS);
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
