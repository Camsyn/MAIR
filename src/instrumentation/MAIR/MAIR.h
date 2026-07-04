#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/simple_ilist.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/raw_ostream.h"

#include "MAIR/MAIRBarrierAnalysis.h"

#include <cstdint>
#include <string>
#include <utility>

namespace mair {
using namespace llvm;

class MAIRGraph;
class MAIRVerifier;
class Observation;

enum class AccessKind : uint8_t { Read = 1, Write = 2, ReadWrite = 3 };

class MemoryAccess final
    : public ilist_node_with_parent<MemoryAccess, Observation,
                                    ilist_parent<Observation>> {
  const Instruction *Inst = nullptr;
  MemoryLocation Loc;
  AccessKind Kind = AccessKind::Read;

public:
  MemoryAccess() = default;
  MemoryAccess(const Instruction *I, MemoryLocation L, AccessKind K)
      : Inst(I), Loc(L), Kind(K) {}

  MemoryAccess(const MemoryAccess &) = delete;
  MemoryAccess &operator=(const MemoryAccess &) = delete;
  MemoryAccess(MemoryAccess &&) = delete;
  MemoryAccess &operator=(MemoryAccess &&) = delete;

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

  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;
};

class Observation final
    : public ilist_node_with_parent<Observation, MAIRGraph,
                                    ilist_parent<MAIRGraph>> {
public:
  enum class SiteKind : uint8_t { MemoryInst, BasicBlockEntry, BasicBlockExit };
  using AccessListType = simple_ilist<MemoryAccess, ilist_parent<Observation>>;
  using iterator = AccessListType::iterator;
  using const_iterator = AccessListType::const_iterator;

private:
  std::string Name;
  const BasicBlock *BB = nullptr;
  const Instruction *AnchorInst = nullptr;
  SiteKind Kind = SiteKind::MemoryInst;
  SpecificBumpPtrAllocator<MemoryAccess> AccessAllocator;
  AccessListType Accesses;
  unsigned NumAccesses = 0;

public:
  Observation() = default;
  Observation(StringRef N, const BasicBlock *Block, const Instruction *Anchor,
              SiteKind K = SiteKind::MemoryInst);
  ~Observation();

  Observation(const Observation &) = delete;
  Observation &operator=(const Observation &) = delete;
  Observation(Observation &&) = delete;
  Observation &operator=(Observation &&) = delete;

  StringRef getName() const { return Name; }
  const BasicBlock *getBasicBlock() const { return BB; }
  const Instruction *getAnchor() const { return AnchorInst; }
  SiteKind getSiteKind() const { return Kind; }

  bool isMemoryInstSite() const { return Kind == SiteKind::MemoryInst; }
  bool isBasicBlockEntrySite() const {
    return Kind == SiteKind::BasicBlockEntry;
  }
  bool isBasicBlockExitSite() const { return Kind == SiteKind::BasicBlockExit; }

  const AccessListType &accesses() const { return Accesses; }
  AccessListType &accesses() { return Accesses; }
  iterator begin() { return Accesses.begin(); }
  iterator end() { return Accesses.end(); }
  const_iterator begin() const { return Accesses.begin(); }
  const_iterator end() const { return Accesses.end(); }
  bool empty() const { return NumAccesses == 0; }
  unsigned size() const { return NumAccesses; }

  MemoryAccess &addAccess(const MemoryAccess &A);
  iterator erase(iterator It);
  void mergeAccessesFrom(const Observation &Other);

  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;

private:
  static AccessListType Observation::*getSublistAccess(MemoryAccess *) {
    return &Observation::Accesses;
  }
  MemoryAccess &createAccess(const Instruction *I, MemoryLocation Loc,
                             AccessKind Kind);
};

class MAIRGraph final {
  friend class MAIRVerifier;

public:
  using ObservationListType =
      simple_ilist<Observation, ilist_parent<MAIRGraph>>;
  using iterator = ObservationListType::iterator;
  using const_iterator = ObservationListType::const_iterator;
  using EdgeListType = SmallVector<Observation *, 2>;
  using ConstEdgeListType = SmallVector<const Observation *, 2>;

private:
  const Function *F = nullptr;
  SpecificBumpPtrAllocator<Observation> GraphAllocator;
  ObservationListType Observations;
  SmallVector<std::pair<Observation *, Observation *>, 8> Edges;
  DenseMap<Observation *, SmallVector<Observation *, 4>> SuccMap;
  DenseMap<Observation *, SmallVector<Observation *, 4>> PredMap;
  unsigned NumObservations = 0;
  unsigned NumEdges = 0;
  unsigned NextObservationID = 0;

public:
  MAIRGraph() = default;
  explicit MAIRGraph(const Function &Func);
  MAIRGraph(const MAIRGraph &) = delete;
  MAIRGraph &operator=(const MAIRGraph &) = delete;
  MAIRGraph(MAIRGraph &&Other) noexcept;
  MAIRGraph &operator=(MAIRGraph &&Other) noexcept;
  ~MAIRGraph();

  const Function *getFunction() const { return F; }
  bool empty() const { return NumObservations == 0; }
  size_t size() const { return NumObservations; }

  /// Build a MAIR graph for \p F using DT/PDT results owned by \p FAM.
  ///
  /// This overload is the pass-pipeline entry point: it must only fetch
  /// reusable LLVM analyses from FAM and then delegate to the explicit-analysis
  /// overload below. It intentionally does not construct DT/PDT itself.
  template <class BarrierTrait = DefaultBarrierTrait>
  static MAIRGraph build(Function &F, FunctionAnalysisManager &FAM) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
    return build<BarrierTrait>(F, DT, PDT);
  }

  /// Build a MAIR graph for \p F using caller-provided dominance analyses.
  ///
  /// This overload owns the BarrierTrait-to-analysis binding. The actual graph
  /// construction stays in the private non-template overload so the heavy
  /// construction algorithm has one implementation.
  template <class BarrierTrait = DefaultBarrierTrait>
  static MAIRGraph build(Function &F, const DominatorTree &DT,
                         const PostDominatorTree &PDT) {
    MAIRBarrierAnalysis<BarrierTrait> BarrierAnalysis;
    return build(F, DT, PDT, BarrierAnalysis);
  }

  iterator begin() { return Observations.begin(); }
  iterator end() { return Observations.end(); }
  const_iterator begin() const { return Observations.begin(); }
  const_iterator end() const { return Observations.end(); }

  Observation &createBBObservation(const BasicBlock &BB, bool IsEntry);
  Observation &createInstObservation(const BasicBlock &BB,
                                     const Instruction &Anchor);

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

  bool verify(raw_ostream *OS = nullptr) const;
  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;
  void dump(raw_ostream &OS) const { print(OS); }

private:
  template <class T, class... Args> T &allocateNode(Args &&...ArgsPack);
  std::string createObservationName(StringRef Prefix);
  Observation &addObservation(Observation &Obs);

  /// Construct the raw MAIR graph once dominance and barrier analyses are
  /// fixed.
  ///
  /// Creates BB-entry, memory-instruction, and BB-exit observations, then adds
  /// intra-block and dominance/post-dominance edges gated by \p
  /// BarrierAnalysis.
  static MAIRGraph build(Function &F, const DominatorTree &DT,
                         const PostDominatorTree &PDT,
                         MAIRBarrierAnalysisCore &BarrierAnalysis);

  static ObservationListType MAIRGraph::*getSublistAccess(Observation *) {
    return &MAIRGraph::Observations;
  }
};

} // namespace mair
