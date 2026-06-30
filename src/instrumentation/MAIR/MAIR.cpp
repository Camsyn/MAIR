#include "MAIR/MAIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/FormattedStream.h"

#include <cstring>

using namespace llvm;

namespace __xsan::mair {

static AccessKind mergeKind(AccessKind LHS, AccessKind RHS) {
  if (LHS == RHS)
    return LHS;
  return AccessKind::ReadWrite;
}

static std::string getPrintedValueName(const Value &V) {
  std::string Name;
  raw_string_ostream OS(Name);
  V.printAsOperand(OS, false);
  OS.flush();
  return Name;
}

static StringRef getPrintedMemoryOpcode(const Instruction &I,
                                        SmallVectorImpl<char> &Storage) {
  std::string InstText;
  raw_string_ostream InstOS(InstText);
  I.print(InstOS);
  InstOS.flush();

  StringRef Text = StringRef(InstText).ltrim();
  if (Text.contains(" load ")) {
    Storage.assign({'l', 'o', 'a', 'd'});
    return StringRef(Storage.data(), Storage.size());
  }
  if (Text.starts_with("store ")) {
    Storage.assign({'s', 't', 'o', 'r', 'e'});
    return StringRef(Storage.data(), Storage.size());
  }
  Storage.assign(I.getOpcodeName(),
                 I.getOpcodeName() + strlen(I.getOpcodeName()));
  return StringRef(Storage.data(), Storage.size());
}

void MemoryAccess::mergeKind(AccessKind Other) {
  Kind = ::__xsan::mair::mergeKind(Kind, Other);
}

std::string MemoryAccess::toString(bool annotate) const {
  (void)annotate;
  std::string S;
  raw_string_ostream OS(S);
  print(OS);
  OS.flush();
  return S;
}

MemoryAccess MemoryAccess::fromInstruction(const Instruction &I,
                                           const DataLayout &DL) {
  (void)DL;
  std::string InstText;
  raw_string_ostream InstOS(InstText);
  I.print(InstOS);
  InstOS.flush();

  StringRef Text = StringRef(InstText).ltrim();
  AccessKind Kind = AccessKind::Read;
  if (Text.contains(" load "))
    Kind = AccessKind::Read;
  else if (Text.starts_with("store "))
    Kind = AccessKind::Write;
  else if (I.mayReadFromMemory() && I.mayWriteToMemory())
    Kind = AccessKind::ReadWrite;
  else if (I.mayWriteToMemory())
    Kind = AccessKind::Write;
  else if (!I.mayReadFromMemory())
    report_fatal_error(
        "MemoryAccess::fromInstruction expects a memory access instruction");

  MemoryLocation Loc = MemoryLocation::get(&I);
  return MemoryAccess(&I, Loc, Kind);
}

void MemoryAccess::print(raw_ostream &OS) const {
  OS << "ma[";
  if (Inst) {
    SmallVector<char, 16> Opcode;
    OS << "inst=" << getPrintedMemoryOpcode(*Inst, Opcode);
    if (Inst->hasName())
      OS << ",name=" << Inst->getName();
  } else {
    OS << "inst=<null>";
  }
  OS << ",kind=";
  switch (Kind) {
  case AccessKind::Read:
    OS << "read";
    break;
  case AccessKind::Write:
    OS << "write";
    break;
  case AccessKind::ReadWrite:
    OS << "readwrite";
    break;
  }
  OS << ",ptr=";
  if (Loc.Ptr)
    OS << getPrintedValueName(*Loc.Ptr);
  else
    OS << "<unknown>";
  OS << ",size=" << Loc.Size << "]";
}

Observation::Observation(StringRef N, const BasicBlock *Block,
                         const Instruction *Anchor, SiteKind K)
    : Name(N.str()), BB(Block), AnchorInst(Anchor), Kind(K) {}

void Observation::addAccess(const MemoryAccess &A) {
  Accesses.push_back(A);
  Accesses.back().setParent(this);
}

void Observation::addAccess(MemoryAccess &&A) {
  Accesses.push_back(std::move(A));
  Accesses.back().setParent(this);
}

void Observation::mergeAccessesFrom(const Observation &Other) {
  for (const MemoryAccess &A : Other.Accesses)
    addAccess(A);
}

std::string Observation::toString(bool annotate) const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS);
  OS.flush();
  return S;
}

void Observation::print(raw_ostream &OS) const {
  OS << "obs " << Name << " {";
  OS << "site=" << (Kind == SiteKind::MemoryInst ? "memory-inst" : "bb-entry");
  if (BB) {
    OS << ",bb=";
    OS << getPrintedValueName(*BB);
  }
  if (AnchorInst) {
    SmallVector<char, 16> Opcode;
    OS << ",anchor=" << getPrintedMemoryOpcode(*AnchorInst, Opcode);
    if (AnchorInst->hasName())
      OS << ":" << AnchorInst->getName();
  }
  OS << ",accesses=[";
  bool First = true;
  for (const MemoryAccess &A : Accesses) {
    if (!First)
      OS << ", ";
    A.print(OS);
    First = false;
  }
  OS << "]}";
}

MAIRGraph::MAIRGraph(const Function &Func) : F(&Func) {}

Observation &MAIRGraph::addObservation(Observation Obs) {
  OwnedObservations.push_back(std::make_unique<Observation>(std::move(Obs)));
  Observation *Node = OwnedObservations.back().get();
  Node->setParent(this);
  for (MemoryAccess &Access : Node->accesses())
    Access.setParent(Node);
  Observations.push_back(*Node);
  return *Node;
}

Observation &MAIRGraph::createObservation(StringRef Name, const BasicBlock *BB,
                                          const Instruction *Anchor,
                                          Observation::SiteKind Kind) {
  return addObservation(Observation(Name, BB, Anchor, Kind));
}

MAIRGraph::iterator MAIRGraph::erase(Observation &Obs) {
  auto It = Obs.getIterator();
  auto Next = Observations.erase(It);
  Edges.erase(llvm::remove_if(Edges,
                              [&Obs](const auto &Edge) {
                                return Edge.first == &Obs ||
                                       Edge.second == &Obs;
                              }),
              Edges.end());
  for (auto OwnedIt = OwnedObservations.begin();
       OwnedIt != OwnedObservations.end(); ++OwnedIt) {
    if (OwnedIt->get() != &Obs)
      continue;
    OwnedObservations.erase(OwnedIt);
    break;
  }
  return Next;
}

void MAIRGraph::clear() {
  Edges.clear();
  Observations.clear();
  OwnedObservations.clear();
}

Observation *MAIRGraph::find(StringRef Name) {
  return const_cast<Observation *>(std::as_const(*this).find(Name));
}

const Observation *MAIRGraph::find(StringRef Name) const {
  for (const Observation &Obs : Observations)
    if (Obs.getName() == Name)
      return &Obs;
  return nullptr;
}

void MAIRGraph::addEdge(Observation &From, Observation &To) {
  for (const auto &Edge : Edges)
    if (Edge.first == &From && Edge.second == &To)
      return;
  Edges.push_back({&From, &To});
}

void MAIRGraph::removeEdge(Observation &From, Observation &To) {
  Edges.erase(llvm::remove_if(Edges,
                              [&From, &To](const auto &Edge) {
                                return Edge.first == &From &&
                                       Edge.second == &To;
                              }),
              Edges.end());
}

MAIRGraph::EdgeListType MAIRGraph::successors(Observation &Obs) {
  EdgeListType Result;
  for (const auto &Edge : Edges)
    if (Edge.first == &Obs)
      Result.push_back(Edge.second);
  return Result;
}

MAIRGraph::ConstEdgeListType
MAIRGraph::successors(const Observation &Obs) const {
  ConstEdgeListType Result;
  for (const auto &Edge : Edges)
    if (Edge.first == &Obs)
      Result.push_back(Edge.second);
  return Result;
}

MAIRGraph::EdgeListType MAIRGraph::predecessors(Observation &Obs) {
  EdgeListType Result;
  for (const auto &Edge : Edges)
    if (Edge.second == &Obs)
      Result.push_back(Edge.first);
  return Result;
}

MAIRGraph::ConstEdgeListType
MAIRGraph::predecessors(const Observation &Obs) const {
  ConstEdgeListType Result;
  for (const auto &Edge : Edges)
    if (Edge.second == &Obs)
      Result.push_back(Edge.first);
  return Result;
}

bool MAIRGraph::verify() const {
  for (const Observation &Obs : Observations) {
    if (Obs.getParent() != this)
      return false;
    if (Obs.isBasicBlockEntrySite() && Obs.getAnchorInstruction())
      return false;
    if (Obs.isMemoryInstSite() && !Obs.getAnchorInstruction())
      return false;
    for (const MemoryAccess &Access : Obs) {
      if (Access.getParent() != &Obs)
        return false;
      if (Obs.isMemoryInstSite() &&
          Access.getInstruction() != Obs.getAnchorInstruction())
        return false;
    }
  }
  for (const auto &Edge : Edges) {
    if (!Edge.first || !Edge.second)
      return false;
    if (Edge.first->getParent() != this || Edge.second->getParent() != this)
      return false;
  }
  return true;
}

std::string MAIRGraph::toString(bool annotate) const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS);
  OS.flush();
  return S;
}

void MAIRGraph::print(raw_ostream &OS) const {
  OS << "mair graph";
  if (F) {
    OS << " function ";
    OS << getPrintedValueName(*F);
  }
  OS << " {\n";
  for (const Observation &Obs : Observations) {
    OS << "  ";
    Obs.print(OS);
    ConstEdgeListType Succs = successors(Obs);
    if (!Succs.empty()) {
      OS << " ; succs=[";
      bool First = true;
      for (const Observation *Succ : Succs) {
        if (!First)
          OS << ", ";
        OS << Succ->getName();
        First = false;
      }
      OS << "]";
    }
    OS << "\n";
  }
  OS << "}\n";
}

void MAIRAnnotatedWriter::emitFunctionAnnot(const Function *F,
                                            formatted_raw_ostream &OS) {
  if (!Graph.getFunction() || Graph.getFunction() != F)
    return;
  OS << " ; MAIR fn";
}

void MAIRAnnotatedWriter::emitBasicBlockStartAnnot(const BasicBlock *BB,
                                                   formatted_raw_ostream &OS) {
  for (const Observation &Obs : Graph) {
    if (Obs.getBasicBlock() == BB && Obs.isBasicBlockEntrySite()) {
      OS << " ; ";
      Obs.print(OS);
    }
  }
}

void MAIRAnnotatedWriter::emitInstructionAnnot(const Instruction *I,
                                               formatted_raw_ostream &OS) {
  for (const Observation &Obs : Graph) {
    if (Obs.getAnchorInstruction() != I)
      continue;
    OS << " ; ";
    Obs.print(OS);
  }
}

void MAIRAnnotatedWriter::printInfoComment(const Value &V,
                                           formatted_raw_ostream &OS) {
  if (const auto *I = dyn_cast<Instruction>(&V))
    emitInstructionAnnot(I, OS);
}

void dumpPure(const MAIRGraph &Graph, raw_ostream &OS) { Graph.print(OS); }

void dumpAnnotated(const MAIRGraph &Graph, const Module &M, raw_ostream &OS) {
  MAIRAnnotatedWriter Writer(Graph);
  M.print(OS, &Writer);
}

} // namespace __xsan::mair
