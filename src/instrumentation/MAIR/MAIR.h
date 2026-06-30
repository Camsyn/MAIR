#pragma once

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace __xsan::mair {
using namespace llvm;

class MAIRGraph;
class Observation;

enum class AccessKind : uint8_t { Read = 1, Write = 2, ReadWrite = 3 };

class MemoryAccess {
  Observation *Parent = nullptr;
  const Instruction *Inst = nullptr;
  MemoryLocation Loc;
  AccessKind Kind = AccessKind::Read;

public:
  MemoryAccess() = default;
  MemoryAccess(const Instruction *I, MemoryLocation L, AccessKind K)
      : Inst(I), Loc(L), Kind(K) {}

  Observation *getParent() { return Parent; }
  const Observation *getParent() const { return Parent; }
  void setParent(Observation *P) { Parent = P; }

  const Instruction *getInstruction() const { return Inst; }
  const MemoryLocation &getLocation() const { return Loc; }
  LocationSize getSize() const { return Loc.Size; }
  const Value *getPtr() const { return Loc.Ptr; }
  AccessKind getKind() const { return Kind; }
  bool isRead() const { return Kind == AccessKind::Read; }
  bool isWrite() const { return Kind == AccessKind::Write; }
  bool isReadWrite() const { return Kind == AccessKind::ReadWrite; }

  void mergeKind(AccessKind Other);

  static MemoryAccess fromInstruction(const Instruction &I,
                                      const DataLayout &DL);

  std::string toString(bool annotate = false) const;
  void print(raw_ostream &OS) const;
};

class Observation final
    : public ilist_node_with_parent<Observation, class MAIRGraph> {
public:
  friend class ilist_traits<Observation>;
  enum class SiteKind : uint8_t { MemoryInst, BasicBlockEntry };
  using AccessListType = SmallVector<MemoryAccess, 4>;
  using iterator = AccessListType::iterator;
  using const_iterator = AccessListType::const_iterator;

private:
  MAIRGraph *Parent = nullptr;
  std::string Name;
  const BasicBlock *BB = nullptr;
  const Instruction *AnchorInst = nullptr;
  SiteKind Kind = SiteKind::MemoryInst;
  AccessListType Accesses;

public:
  Observation() = default;
  Observation(StringRef N, const BasicBlock *Block, const Instruction *Anchor,
              SiteKind K = SiteKind::MemoryInst);

  MAIRGraph *getParent() { return Parent; }
  const MAIRGraph *getParent() const { return Parent; }
  void setParent(MAIRGraph *P) { Parent = P; }

  StringRef getName() const { return Name; }
  const BasicBlock *getBasicBlock() const { return BB; }
  const Instruction *getAnchorInstruction() const { return AnchorInst; }
  SiteKind getSiteKind() const { return Kind; }

  bool isMemoryInstSite() const { return Kind == SiteKind::MemoryInst; }
  bool isBasicBlockEntrySite() const {
    return Kind == SiteKind::BasicBlockEntry;
  }

  ArrayRef<MemoryAccess> accesses() const { return Accesses; }
  SmallVectorImpl<MemoryAccess> &accesses() { return Accesses; }
  iterator begin() { return Accesses.begin(); }
  iterator end() { return Accesses.end(); }
  const_iterator begin() const { return Accesses.begin(); }
  const_iterator end() const { return Accesses.end(); }

  void addAccess(const MemoryAccess &A);
  void addAccess(MemoryAccess &&A);
  iterator erase(iterator It) { return Accesses.erase(It); }
  void mergeAccessesFrom(const Observation &Other);

  bool empty() const { return Accesses.empty(); }
  std::string toString(bool annotate = false) const;
  void print(raw_ostream &OS) const;
};

class MAIRGraph final {
public:
  using ObservationListType = simple_ilist<Observation>;
  using iterator = ObservationListType::iterator;
  using const_iterator = ObservationListType::const_iterator;
  using EdgeListType = SmallVector<Observation *, 2>;
  using ConstEdgeListType = SmallVector<const Observation *, 2>;

private:
  const Function *F = nullptr;
  ObservationListType Observations;
  std::vector<std::unique_ptr<Observation>> OwnedObservations;
  SmallVector<std::pair<Observation *, Observation *>, 8> Edges;

public:
  MAIRGraph() = default;
  explicit MAIRGraph(const Function &Func);

  const Function *getFunction() const { return F; }
  bool empty() const { return Observations.empty(); }
  size_t size() const { return Observations.size(); }

  iterator begin() { return Observations.begin(); }
  iterator end() { return Observations.end(); }
  const_iterator begin() const { return Observations.begin(); }
  const_iterator end() const { return Observations.end(); }

  Observation &addObservation(Observation Obs);
  Observation &createObservation(
      StringRef Name, const BasicBlock *BB, const Instruction *Anchor,
      Observation::SiteKind Kind = Observation::SiteKind::MemoryInst);

  iterator erase(Observation &Obs);
  void clear();

  Observation *find(StringRef Name);
  const Observation *find(StringRef Name) const;

  void addEdge(Observation &From, Observation &To);
  void removeEdge(Observation &From, Observation &To);
  EdgeListType successors(Observation &Obs);
  ConstEdgeListType successors(const Observation &Obs) const;
  EdgeListType predecessors(Observation &Obs);
  ConstEdgeListType predecessors(const Observation &Obs) const;

  bool verify() const;
  std::string toString(bool annotate = false) const;
  void print(raw_ostream &OS) const;
  void dump(raw_ostream &OS) const { print(OS); }

  static ObservationListType MAIRGraph::*getSublistAccess(Observation *) {
    return &MAIRGraph::Observations;
  }
};

class MAIRAnnotatedWriter final : public AssemblyAnnotationWriter {
  const MAIRGraph &Graph;

public:
  explicit MAIRAnnotatedWriter(const MAIRGraph &G) : Graph(G) {}

  void emitFunctionAnnot(const Function *F, formatted_raw_ostream &OS) override;
  void emitBasicBlockStartAnnot(const BasicBlock *BB,
                                formatted_raw_ostream &OS) override;
  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &OS) override;
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override;
};

void dumpPure(const MAIRGraph &Graph, raw_ostream &OS);
void dumpAnnotated(const MAIRGraph &Graph, const Module &M, raw_ostream &OS);

} // namespace __xsan::mair
