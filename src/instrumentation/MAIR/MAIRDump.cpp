#include "MAIR/MAIRDump.h"

#include "llvm/ADT/DenseMap.h"
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

struct MAIRGraphDOTView {
  const MAIRGraph &Graph;
  SmallVector<const ProgramPoint *, 16> Nodes;

  explicit MAIRGraphDOTView(const MAIRGraph &Graph)
      : Graph(Graph), Nodes(Graph.allProgramPoints()) {}
};

static StringRef getProgramPointSiteKindName(ProgramPoint::SiteKind Kind) {
  switch (Kind) {
  case ProgramPoint::SiteKind::MemoryInst:
    return "memory";
  case ProgramPoint::SiteKind::BasicBlockEntry:
    return "bb.entry";
  case ProgramPoint::SiteKind::BasicBlockExit:
    return "bb.exit";
  case ProgramPoint::SiteKind::Edge:
    return "edge";
  }
  llvm_unreachable("unknown MAIR program point site kind");
}

static StringRef getProgramPointEdgeKindName(PPEdgeKind Kind) {
  switch (Kind) {
  case PPEdgeKind::Dom:
    return "dom";
  case PPEdgeKind::PostDom:
    return "pdom";
  }
  llvm_unreachable("unknown MAIR program point edge kind");
}

namespace {
class MAIRLLVMWriter final : public AssemblyAnnotationWriter {
  DenseMap<const Instruction *, SmallVector<const ProgramPoint *, 2>>
      AnchoredProgramPoints;
  void emitAnchoredProgramPoints(const Instruction *I,
                                 formatted_raw_ostream &OS) const;
  void emitProgramPointLine(const ProgramPoint &PP,
                            formatted_raw_ostream &OS) const;

public:
  explicit MAIRLLVMWriter(ArrayRef<const MAIRGraph *> Graphs) {
    for (const MAIRGraph *Graph : Graphs)
      if (Graph)
        for (const ProgramPoint *PP : Graph->allProgramPoints())
          AnchoredProgramPoints[&PP->getAnchor()].push_back(PP);
  }

  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &OS) override;
};
} // namespace

} // namespace mair

namespace llvm {

template <>
struct GraphTraits<const mair::MAIRGraphDOTView *>
    : GraphTraits<const mair::ProgramPoint *> {
  using nodes_iterator =
      SmallVector<const mair::ProgramPoint *, 16>::const_iterator;

  static NodeRef getEntryNode(const mair::MAIRGraphDOTView *View) {
    return View->Nodes.empty() ? nullptr : View->Nodes.front();
  }
  static nodes_iterator nodes_begin(const mair::MAIRGraphDOTView *View) {
    return View->Nodes.begin();
  }
  static nodes_iterator nodes_end(const mair::MAIRGraphDOTView *View) {
    return View->Nodes.end();
  }
  static size_t size(const mair::MAIRGraphDOTView *View) {
    return View->Nodes.size();
  }
};

template <>
struct DOTGraphTraits<const mair::MAIRGraphDOTView *>
    : public DefaultDOTGraphTraits {
  using ChildIteratorType =
      GraphTraits<const mair::ProgramPoint *>::ChildIteratorType;

  explicit DOTGraphTraits(bool IsSimple = false)
      : DefaultDOTGraphTraits(IsSimple) {}

  static std::string getGraphName(const mair::MAIRGraphDOTView *View) {
    if (View && View->Graph.getFunction())
      return ("MAIRGraph for " + View->Graph.getFunction()->getName()).str();
    return "MAIRGraph";
  }

  std::string getNodeLabel(const mair::ProgramPoint *PP,
                           const mair::MAIRGraphDOTView *) {
    std::string Label;
    raw_string_ostream OS(Label);
    OS << PP->getName() << "\\n";
    OS << mair::getProgramPointSiteKindName(PP->getSiteKind());
    if (const auto *LoopPP = dyn_cast<mair::LoopProgramPoint>(PP))
      OS << (LoopPP->isLoopPreheader() ? " loop.preheader" : " loop.exit");
    if (const BasicBlock *BB = PP->getBasicBlock())
      OS << " in " << BB->getName();
    if (PP->isEdgeSite() || !PP->hasAlwaysGuard()) {
      OS << "\\n";
      PP->getSite().print(OS);
    }
    if (const auto *Guard =
            std::get_if<mair::AnyProvenanceExecutedGuard>(&PP->getGuard())) {
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
    for (const mair::Observation &Obs : PP->observations())
      OS << "\\n" << Obs.toString();
    return OS.str();
  }

  static std::string getNodeAttributes(const mair::ProgramPoint *PP,
                                       const mair::MAIRGraphDOTView *) {
    switch (PP->getSiteKind()) {
    case mair::ProgramPoint::SiteKind::MemoryInst:
      return "style=filled, fillcolor=lightyellow";
    case mair::ProgramPoint::SiteKind::BasicBlockEntry:
      return "style=filled, fillcolor=lightcyan";
    case mair::ProgramPoint::SiteKind::BasicBlockExit:
      return "style=filled, fillcolor=lightgray";
    case mair::ProgramPoint::SiteKind::Edge:
      return "style=filled, fillcolor=mistyrose";
    }
    llvm_unreachable("unknown MAIR program point site kind");
  }

  static std::string getEdgeAttributes(const mair::ProgramPoint *,
                                       ChildIteratorType I,
                                       const mair::MAIRGraphDOTView *) {
    std::string Attrs;
    raw_string_ostream OS(Attrs);
    mair::PPEdgeKind Kind = I.getKind();
    OS << "label=\"" << mair::getProgramPointEdgeKindName(Kind) << "\"";
    if (Kind == mair::PPEdgeKind::Dom)
      OS << ", color=blue";
    else
      OS << ", color=red";
    return OS.str();
  }
};

} // namespace llvm

namespace mair {

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

void MAIRLLVMWriter::emitAnchoredProgramPoints(
    const Instruction *I, formatted_raw_ostream &OS) const {
  auto Points = AnchoredProgramPoints.find(I);
  if (Points == AnchoredProgramPoints.end())
    return;
  for (const ProgramPoint *PP : Points->second)
    emitProgramPointLine(*PP, OS);
}

void MAIRLLVMWriter::emitProgramPointLine(const ProgramPoint &PP,
                                          formatted_raw_ostream &OS) const {
  std::string Text = PP.toString();
  SmallVector<StringRef, 4> Lines;
  StringRef(Text).split(Lines, '\n');
  for (StringRef Line : Lines)
    if (!Line.trim().empty())
      OS << "  ; " << Line << "\n";
}

void MAIRLLVMWriter::emitInstructionAnnot(const Instruction *I,
                                          formatted_raw_ostream &OS) {
  emitAnchoredProgramPoints(I, OS);
}

void dumpMAIR(const MAIRGraph &Graph, raw_ostream &OS) { Graph.print(OS); }

void dumpDOT(const MAIRGraph &Graph, raw_ostream &OS) {
  const MAIRGraphDOTView View(Graph);
  WriteGraph(OS, &View, /*ShortNames=*/false);
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
