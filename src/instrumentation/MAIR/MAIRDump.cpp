#include "MAIR/MAIRDump.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace mair {

static StringRef getObservationSiteKindName(Observation::SiteKind Kind) {
  switch (Kind) {
  case Observation::SiteKind::MemoryInst:
    return "memory";
  case Observation::SiteKind::BasicBlockEntry:
    return "bb.entry";
  case Observation::SiteKind::BasicBlockExit:
    return "bb.exit";
  case Observation::SiteKind::Edge:
    return "edge";
  }
  llvm_unreachable("unknown MAIR observation site kind");
}

static StringRef getObservationEdgeKindName(ObsEdgeKind Kind) {
  switch (Kind) {
  case ObsEdgeKind::Dom:
    return "dom";
  case ObsEdgeKind::PostDom:
    return "pdom";
  }
  llvm_unreachable("unknown MAIR observation edge kind");
}

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

  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &OS) override;
};
} // namespace

} // namespace mair

namespace llvm {

template <>
struct DOTGraphTraits<const mair::MAIRGraph *> : public DefaultDOTGraphTraits {
  using ChildIteratorType =
      GraphTraits<const mair::Observation *>::ChildIteratorType;

  explicit DOTGraphTraits(bool IsSimple = false)
      : DefaultDOTGraphTraits(IsSimple) {}

  static std::string getGraphName(const mair::MAIRGraph *Graph) {
    if (Graph && Graph->getFunction())
      return ("MAIRGraph for " + Graph->getFunction()->getName()).str();
    return "MAIRGraph";
  }

  std::string getNodeLabel(const mair::Observation *Obs,
                           const mair::MAIRGraph *) {
    std::string Label;
    raw_string_ostream OS(Label);
    OS << Obs->getName() << "\\n";
    OS << mair::getObservationSiteKindName(Obs->getSiteKind());
    if (const BasicBlock *BB = Obs->getBasicBlock())
      OS << " in " << BB->getName();
    if (Obs->isEdgeSite() || !Obs->hasAlwaysGuard()) {
      OS << "\\n";
      Obs->getSite().print(OS);
    }
    if (const auto *Guard =
            std::get_if<mair::AnyProvenanceExecutedGuard>(&Obs->getGuard())) {
      OS << "\\nguard=any(";
      bool First = true;
      for (const Instruction *I : Guard->getProvenance()) {
        if (!First)
          OS << ", ";
        if (I)
          I->printAsOperand(OS, false);
        else
          OS << "null";
        First = false;
      }
      OS << ")";
    }
    for (const mair::MemoryAccess &Access : Obs->accesses())
      OS << "\\n" << Access.toString();
    return OS.str();
  }

  static std::string getNodeAttributes(const mair::Observation *Obs,
                                       const mair::MAIRGraph *) {
    switch (Obs->getSiteKind()) {
    case mair::Observation::SiteKind::MemoryInst:
      return "style=filled, fillcolor=lightyellow";
    case mair::Observation::SiteKind::BasicBlockEntry:
      return "style=filled, fillcolor=lightcyan";
    case mair::Observation::SiteKind::BasicBlockExit:
      return "style=filled, fillcolor=lightgray";
    case mair::Observation::SiteKind::Edge:
      return "style=filled, fillcolor=mistyrose";
    }
    llvm_unreachable("unknown MAIR observation site kind");
  }

  static std::string getEdgeAttributes(const mair::Observation *,
                                       ChildIteratorType I,
                                       const mair::MAIRGraph *) {
    std::string Attrs;
    raw_string_ostream OS(Attrs);
    mair::ObsEdgeKind Kind = I.getKind();
    OS << "label=\"" << mair::getObservationEdgeKindName(Kind) << "\"";
    if (Kind == mair::ObsEdgeKind::Dom)
      OS << ", color=blue";
    else
      OS << ", color=red";
    return OS.str();
  }
};

} // namespace llvm

namespace mair {

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

void MAIRLLVMWriter::emitAnchoredObservations(const Instruction *I,
                                              formatted_raw_ostream &OS) const {
  const MAIRGraph *Graph = findGraph(I->getFunction());
  if (!Graph)
    return;
  for (const Observation &Obs : *Graph)
    if (&Obs.getAnchor() == I)
      emitObservationLine(Obs, OS);
}

void MAIRLLVMWriter::emitObservationLine(const Observation &Obs,
                                         formatted_raw_ostream &OS) const {
  std::string Text = Obs.toString();
  SmallVector<StringRef, 4> Lines;
  StringRef(Text).split(Lines, '\n');
  for (StringRef Line : Lines)
    if (!Line.trim().empty())
      OS << "  ; " << Line << "\n";
}

void MAIRLLVMWriter::emitInstructionAnnot(const Instruction *I,
                                          formatted_raw_ostream &OS) {
  emitAnchoredObservations(I, OS);
}

void dumpMAIR(const MAIRGraph &Graph, raw_ostream &OS) { Graph.print(OS); }

void dumpDOT(const MAIRGraph &Graph, raw_ostream &OS) {
  WriteGraph(OS, &Graph, /*ShortNames=*/false);
}

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
