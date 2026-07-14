#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/iterator.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/ADT/simple_ilist.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/raw_ostream.h"

#include "MAIR/MAIRBarrierAnalysis.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace mair {
using namespace llvm;

class MAIRGraph;
class MAIRVerifier;
class MemoryAccess;
class MemoryObject;
class MemoryUse;
class Observation;
class MAIRScalarExpr;
struct MAIRScalarExprNode;
struct MAIRScalarExprOwner;

enum class AccessKind : uint8_t { Read = 1, Write = 2, ReadWrite = 3 };
enum class ObsEdgeKind : uint8_t { Dom, PostDom };

class AbstractValue {
public:
  enum class Kind : uint8_t { Unknown, Symbol, Constant };

private:
  using StorageT = std::variant<std::monostate, const Value *, uint64_t>;
  StorageT Storage;

public:
  AbstractValue() = default;

  static AbstractValue unknown() { return AbstractValue(); }
  static AbstractValue symbol(const Value &V);
  static AbstractValue constant(uint64_t C);

  Kind getKind() const;
  bool isUnknown() const {
    return std::holds_alternative<std::monostate>(Storage);
  }
  bool isSymbol() const {
    return std::holds_alternative<const Value *>(Storage);
  }
  bool isConstant() const { return std::holds_alternative<uint64_t>(Storage); }
  const Value *getSymbol() const {
    assert(isSymbol() && "abstract value is not symbolic");
    return std::get<const Value *>(Storage);
  }
  uint64_t getConstant() const {
    assert(isConstant() && "abstract value is not constant");
    return std::get<uint64_t>(Storage);
  }

  bool operator==(const AbstractValue &Other) const {
    return Storage == Other.Storage;
  }
  bool operator!=(const AbstractValue &Other) const {
    return !(*this == Other);
  }
};

class AbstractAccess {
  const Value *Base = nullptr;
  AbstractValue Offset;
  AbstractValue Size;

public:
  AbstractAccess() = default;
  AbstractAccess(const Value *B, AbstractValue O, AbstractValue S)
      : Base(B), Offset(O), Size(S) {}

  const Value *getBase() const { return Base; }
  const AbstractValue &getOffset() const { return Offset; }
  const AbstractValue &getSize() const { return Size; }

  bool contains(const AbstractAccess &Other) const;

  static AbstractAccess fromPointerAndSize(const Value &Ptr, AbstractValue Size,
                                           const DataLayout &DL);
  static std::optional<AbstractAccess> merge(const AbstractAccess &A,
                                             const AbstractAccess &B);

  bool operator==(const AbstractAccess &Other) const {
    return Base == Other.Base && Offset == Other.Offset && Size == Other.Size;
  }
  bool operator!=(const AbstractAccess &Other) const {
    return !(*this == Other);
  }

  void print(raw_ostream &OS) const;
};

/// A physical placement for one observation event. Edge sites remain virtual
/// until instantiate() creates a dedicated CFG block in a later stage.
class ObservationSite {
public:
  enum class Kind : uint8_t {
    MemoryInst,
    BasicBlockEntry,
    BasicBlockExit,
    Edge
  };

private:
  Kind SiteKind = Kind::MemoryInst;
  const BasicBlock *BB = nullptr;
  const Instruction *Anchor = nullptr;
  const BasicBlock *From = nullptr;
  const BasicBlock *To = nullptr;

public:
  ObservationSite() = default;

  static ObservationSite anchored(const BasicBlock *BB,
                                  const Instruction *Anchor, Kind K);
  static ObservationSite edge(const BasicBlock &From, const BasicBlock &To);

  Kind getKind() const { return SiteKind; }
  bool isAnchored() const { return SiteKind != Kind::Edge; }
  bool isEdge() const { return SiteKind == Kind::Edge; }
  const BasicBlock *getBasicBlock() const {
    return isAnchored() ? BB : nullptr;
  }
  const Instruction *getAnchor() const {
    return isAnchored() ? Anchor : nullptr;
  }
  const BasicBlock *getEdgeFrom() const { return isEdge() ? From : nullptr; }
  const BasicBlock *getEdgeTo() const { return isEdge() ? To : nullptr; }

  bool operator==(const ObservationSite &Other) const {
    return SiteKind == Other.SiteKind && BB == Other.BB &&
           Anchor == Other.Anchor && From == Other.From && To == Other.To;
  }
  bool operator!=(const ObservationSite &Other) const {
    return !(*this == Other);
  }

  void print(raw_ostream &OS) const;
};

/// Execute the observation whenever control reaches its site.
struct AlwaysGuard {
  bool operator==(const AlwaysGuard &) const { return true; }
  bool operator!=(const AlwaysGuard &) const { return false; }
};

/// Execute only if at least one represented source instruction ran. The
/// instruction list has set semantics; storage order is retained for dumps.
class AnyProvenanceExecutedGuard {
  SmallVector<Instruction *, 2> Provenance;

public:
  AnyProvenanceExecutedGuard() = default;
  explicit AnyProvenanceExecutedGuard(ArrayRef<Instruction *> Instructions)
      : Provenance(Instructions) {}

  ArrayRef<Instruction *> getProvenance() const { return Provenance; }
  bool operator==(const AnyProvenanceExecutedGuard &Other) const;
  bool operator!=(const AnyProvenanceExecutedGuard &Other) const {
    return !(*this == Other);
  }
};

using ObservationGuard = std::variant<AlwaysGuard, AnyProvenanceExecutedGuard>;

/// An immutable handle into a MAIRGraph-owned typed expression arena.
/// Expressions store materializable semantics, never Loop or SCEV objects.
class MAIRScalarExpr {
public:
  enum class Kind : uint8_t {
    Constant,
    LLVMValue,
    Add,
    Mul,
    UDiv,
    Truncate,
    ZeroExtend,
    SignExtend,
    SMin,
    UMin,
    SMax,
    UMax,
  };

private:
  const MAIRScalarExprNode *Node = nullptr;

  explicit MAIRScalarExpr(const MAIRScalarExprNode &N) : Node(&N) {}

public:
  MAIRScalarExpr() = default;

  bool isValid() const { return Node != nullptr; }
  Kind getKind() const;
  Type *getType() const;
  const APInt &getConstant() const;
  const Value *getLLVMValue() const;
  ArrayRef<MAIRScalarExpr> operands() const;
  const void *getOpaqueNode() const { return Node; }

  bool operator==(const MAIRScalarExpr &Other) const;
  bool operator!=(const MAIRScalarExpr &Other) const {
    return !(*this == Other);
  }

  void print(raw_ostream &OS, bool Signed = false) const;

private:
  friend class MAIRGraph;
  friend class MAIRVerifier;
  friend struct PeriodicRegion;

  const MAIRScalarExprOwner *getOwner() const;
};

/// The bytes touched by Start + i * Stride for 0 <= i < TripCount.
/// Base is only the canonical MemoryObject identity; Start is the complete
/// address of iteration zero. A zero TripCount denotes the empty region.
struct PeriodicRegion {
  const Value *Base = nullptr;
  MAIRScalarExpr Start;
  MAIRScalarExpr Stride;
  MAIRScalarExpr AccessSize;
  MAIRScalarExpr TripCount;

  PeriodicRegion() = default;
  PeriodicRegion(const Value *B, MAIRScalarExpr S, MAIRScalarExpr Step,
                 MAIRScalarExpr Size, MAIRScalarExpr Trips)
      : Base(B), Start(S), Stride(Step), AccessSize(Size), TripCount(Trips) {}

  const Value *getBase() const { return Base; }
  const MAIRScalarExpr &getStart() const { return Start; }
  const MAIRScalarExpr &getStride() const { return Stride; }
  const MAIRScalarExpr &getAccessSize() const { return AccessSize; }
  const MAIRScalarExpr &getTripCount() const { return TripCount; }

  bool operator==(const PeriodicRegion &Other) const {
    return Base == Other.Base && Start == Other.Start &&
           Stride == Other.Stride && AccessSize == Other.AccessSize &&
           TripCount == Other.TripCount;
  }
  bool operator!=(const PeriodicRegion &Other) const {
    return !(*this == Other);
  }

  void print(raw_ostream &OS) const;
};

/// A contiguous or explicitly periodic memory-access region.
using MemoryRegion = std::variant<AbstractAccess, PeriodicRegion>;

class ObservationEdge {
  Observation *Node = nullptr;
  ObsEdgeKind Kind = ObsEdgeKind::Dom;

public:
  ObservationEdge() = default;
  ObservationEdge(Observation &N, ObsEdgeKind K) : Node(&N), Kind(K) {}

  Observation &getNode() const {
    assert(Node && "MAIR observation edge has no endpoint");
    return *Node;
  }
  ObsEdgeKind getKind() const { return Kind; }
  bool isDom() const { return Kind == ObsEdgeKind::Dom; }
  bool isPostDom() const { return Kind == ObsEdgeKind::PostDom; }
};

/// The operand edge from a MemoryAccess user to a MemoryObject, analogous to
/// llvm::Use. It maintains the object's intrusive use-list.
class MemoryUse {
  friend class MemoryAccess;
  friend class MAIRGraph;

  MemoryObject *Object = nullptr;
  MemoryUse *Next = nullptr;
  MemoryUse **Prev = nullptr;
  MemoryAccess *Parent = nullptr;

public:
  MemoryUse() = default;
  explicit MemoryUse(MemoryAccess &P) : Parent(&P) {}
  ~MemoryUse() { set(nullptr); }

  MemoryUse(const MemoryUse &) = delete;
  MemoryUse &operator=(const MemoryUse &) = delete;
  MemoryUse(MemoryUse &&) = delete;
  MemoryUse &operator=(MemoryUse &&) = delete;

  MemoryObject *get() const { return Object; }
  MemoryAccess *getUser() const { return Parent; }
  MemoryUse *getNext() const { return Next; }

private:
  void set(MemoryObject *NewObject);
  void addToList(MemoryUse **List);
  void removeFromList();
};

template <class UseT> class MemoryUseIterator {
  UseT *Use = nullptr;

public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = std::remove_const_t<UseT>;
  using difference_type = std::ptrdiff_t;
  using pointer = UseT *;
  using reference = UseT &;

  MemoryUseIterator() = default;
  explicit MemoryUseIterator(UseT *U) : Use(U) {}

  reference operator*() const { return *Use; }
  pointer operator->() const { return Use; }
  MemoryUseIterator &operator++() {
    Use = Use->getNext();
    return *this;
  }
  MemoryUseIterator operator++(int) {
    MemoryUseIterator Tmp = *this;
    ++*this;
    return Tmp;
  }
  bool operator==(const MemoryUseIterator &Other) const {
    return Use == Other.Use;
  }
  bool operator!=(const MemoryUseIterator &Other) const {
    return !(*this == Other);
  }
};

struct MemoryUseToUser {
  MemoryAccess &operator()(MemoryUse &Use) const { return *Use.getUser(); }
  const MemoryAccess &operator()(const MemoryUse &Use) const {
    return *Use.getUser();
  }
};

/// A graph-owned wrapper for one LLVM base pointer, analogous to llvm::Value.
/// Memory accesses referring to the base are reachable through its use-list.
class MemoryObject {
  friend class MemoryUse;

  const Value *Base = nullptr;
  MemoryUse *UseList = nullptr;

public:
  using use_iterator = MemoryUseIterator<MemoryUse>;
  using const_use_iterator = MemoryUseIterator<const MemoryUse>;
  using user_iterator = mapped_iterator<use_iterator, MemoryUseToUser>;
  using const_user_iterator =
      mapped_iterator<const_use_iterator, MemoryUseToUser>;

  explicit MemoryObject(const Value &B) : Base(&B) {}
  ~MemoryObject() {
    assert(use_empty() && "destroying a MAIR memory object with uses");
  }

  MemoryObject(const MemoryObject &) = delete;
  MemoryObject &operator=(const MemoryObject &) = delete;
  MemoryObject(MemoryObject &&) = delete;
  MemoryObject &operator=(MemoryObject &&) = delete;

  const Value *getBase() const { return Base; }
  bool use_empty() const { return UseList == nullptr; }

  use_iterator use_begin() { return use_iterator(UseList); }
  use_iterator use_end() { return use_iterator(); }
  const_use_iterator use_begin() const { return const_use_iterator(UseList); }
  const_use_iterator use_end() const { return const_use_iterator(); }
  auto uses() { return make_range(use_begin(), use_end()); }
  auto uses() const { return make_range(use_begin(), use_end()); }

  user_iterator user_begin() {
    return user_iterator(use_begin(), MemoryUseToUser());
  }
  user_iterator user_end() {
    return user_iterator(use_end(), MemoryUseToUser());
  }
  const_user_iterator user_begin() const {
    return const_user_iterator(use_begin(), MemoryUseToUser());
  }
  const_user_iterator user_end() const {
    return const_user_iterator(use_end(), MemoryUseToUser());
  }
  auto users() { return make_range(user_begin(), user_end()); }
  auto users() const { return make_range(user_begin(), user_end()); }
};

template <class NodeT> class ObservationEdgeIterator {
  ArrayRef<ObservationEdge>::iterator It = nullptr;

public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = NodeT *;
  using difference_type = std::ptrdiff_t;
  using pointer = NodeT **;
  using reference = NodeT *;

  ObservationEdgeIterator() = default;
  explicit ObservationEdgeIterator(ArrayRef<ObservationEdge>::iterator I)
      : It(I) {}

  reference operator*() const { return &It->getNode(); }
  ObservationEdgeIterator &operator++() {
    ++It;
    return *this;
  }
  ObservationEdgeIterator operator++(int) {
    ObservationEdgeIterator Tmp = *this;
    ++*this;
    return Tmp;
  }
  bool operator==(const ObservationEdgeIterator &Other) const {
    return It == Other.It;
  }
  bool operator!=(const ObservationEdgeIterator &Other) const {
    return !(*this == Other);
  }

  ObsEdgeKind getKind() const { return It->getKind(); }
};

class MemoryAccess final
    : public ilist_node_with_parent<MemoryAccess, Observation,
                                    ilist_parent<Observation>> {
  SmallVector<Instruction *, 2> Insts;
  MemoryRegion Region;
  AccessKind Kind = AccessKind::Read;
  /// Fixed base operand, analogous to a single llvm::User operand.
  MemoryUse BaseUse;

public:
  MemoryAccess() = default;
  MemoryAccess(ArrayRef<Instruction *> I, AbstractAccess A, AccessKind K)
      : MemoryAccess(I, MemoryRegion(std::move(A)), K) {}
  MemoryAccess(ArrayRef<Instruction *> I, MemoryRegion R, AccessKind K)
      : Insts(I.begin(), I.end()), Region(std::move(R)), Kind(K),
        BaseUse(*this) {}

  MemoryAccess(const MemoryAccess &) = delete;
  MemoryAccess &operator=(const MemoryAccess &) = delete;
  MemoryAccess(MemoryAccess &&) = delete;
  MemoryAccess &operator=(MemoryAccess &&) = delete;

  ArrayRef<Instruction *> getInstructions() const { return Insts; }
  const Instruction *getInstruction() const {
    return Insts.empty() ? nullptr : Insts.front();
  }
  const MemoryRegion &getRegion() const { return Region; }
  const AbstractAccess *getContiguousRegion() const {
    return std::get_if<AbstractAccess>(&Region);
  }
  const PeriodicRegion *getPeriodicRegion() const {
    return std::get_if<PeriodicRegion>(&Region);
  }
  bool isContiguous() const {
    return std::holds_alternative<AbstractAccess>(Region);
  }
  bool isPeriodic() const {
    return std::holds_alternative<PeriodicRegion>(Region);
  }
  MemoryObject &getMemoryObject() {
    assert(BaseUse.get() && "MAIR memory access has no memory object");
    return *BaseUse.get();
  }
  const MemoryObject &getMemoryObject() const {
    assert(BaseUse.get() && "MAIR memory access has no memory object");
    return *BaseUse.get();
  }
  const MemoryUse &getBaseUse() const { return BaseUse; }
  const Value *getBase() const;
  AccessKind getKind() const { return Kind; }
  bool isRead() const { return Kind == AccessKind::Read; }
  bool isWrite() const { return Kind == AccessKind::Write; }
  bool isReadWrite() const { return Kind == AccessKind::ReadWrite; }

  bool mergeWith(const MemoryAccess &Other);
  void mergeKind(AccessKind Other);

  static MemoryAccess fromInstruction(const Instruction &I,
                                      const DataLayout &DL);

  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;

private:
  friend class MAIRGraph;
  friend class Observation;

  void setMemoryObject(MemoryObject *Object) { BaseUse.set(Object); }
  void clearMemoryObject() { BaseUse.set(nullptr); }
};

class Observation final
    : public ilist_node_with_parent<Observation, MAIRGraph,
                                    ilist_parent<MAIRGraph>> {
public:
  using SiteKind = ObservationSite::Kind;
  using AccessListType = simple_ilist<MemoryAccess, ilist_parent<Observation>>;
  using iterator = AccessListType::iterator;
  using const_iterator = AccessListType::const_iterator;
  using EdgeListType = SmallVector<ObservationEdge, 4>;

private:
  std::string Name;
  ObservationSite Site;
  ObservationGuard Guard = AlwaysGuard{};
  SpecificBumpPtrAllocator<MemoryAccess> AccessAllocator;
  AccessListType Accesses;
  EdgeListType Children;
  EdgeListType Parents;
  SmallPtrSet<const BasicBlock *, 2> RepresentedBlocks;
  unsigned NumAccesses = 0;

public:
  Observation() = default;
  Observation(StringRef N, const BasicBlock *Block, const Instruction *Anchor,
              SiteKind K = SiteKind::MemoryInst);
  Observation(StringRef N, ObservationSite S, ObservationGuard G);
  ~Observation();

  Observation(const Observation &) = delete;
  Observation &operator=(const Observation &) = delete;
  Observation(Observation &&) = delete;
  Observation &operator=(Observation &&) = delete;

  StringRef getName() const { return Name; }
  const ObservationSite &getSite() const { return Site; }
  const ObservationGuard &getGuard() const { return Guard; }
  const BasicBlock *getBasicBlock() const { return Site.getBasicBlock(); }
  const Instruction *getAnchor() const { return Site.getAnchor(); }
  const BasicBlock *getEdgeFrom() const { return Site.getEdgeFrom(); }
  const BasicBlock *getEdgeTo() const { return Site.getEdgeTo(); }
  SiteKind getSiteKind() const { return Site.getKind(); }

  bool isMemoryInstSite() const {
    return getSiteKind() == SiteKind::MemoryInst;
  }
  bool isBasicBlockEntrySite() const {
    return getSiteKind() == SiteKind::BasicBlockEntry;
  }
  bool isBasicBlockExitSite() const {
    return getSiteKind() == SiteKind::BasicBlockExit;
  }
  bool isEdgeSite() const { return Site.isEdge(); }
  bool hasAlwaysGuard() const {
    return std::holds_alternative<AlwaysGuard>(Guard);
  }
  bool hasExecutionGuard() const {
    return std::holds_alternative<AnyProvenanceExecutedGuard>(Guard);
  }
  bool representsBlock(const BasicBlock &Block) const {
    return RepresentedBlocks.contains(&Block);
  }

  const AccessListType &accesses() const { return Accesses; }
  AccessListType &accesses() { return Accesses; }
  ArrayRef<ObservationEdge> children() const { return Children; }
  ArrayRef<ObservationEdge> parents() const { return Parents; }
  iterator begin() { return Accesses.begin(); }
  iterator end() { return Accesses.end(); }
  const_iterator begin() const { return Accesses.begin(); }
  const_iterator end() const { return Accesses.end(); }
  bool empty() const { return NumAccesses == 0; }
  unsigned size() const { return NumAccesses; }
  std::optional<ObsEdgeKind> hasChild(const Observation &Obs);
  std::optional<ObsEdgeKind> hasParent(const Observation &Obs);
  /// Merging preserves execution semantics only for equal guards and equal
  /// anchored kinds, or for observations on the exact same CFG edge.
  bool hasCompatibleMergeSemantics(const Observation &Other) const;

  MemoryAccess &addAccess(const MemoryAccess &A);
  iterator erase(iterator It);
  void mergeAccessesFrom(const Observation &Other);

  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;

private:
  friend class MAIRGraph;
  friend class MAIRVerifier;

  static std::optional<ObsEdgeKind> hasEdgeTo(ArrayRef<ObservationEdge> Edges,
                                              const Observation &Obs);

  static AccessListType Observation::*getSublistAccess(MemoryAccess *) {
    return &Observation::Accesses;
  }
  MemoryAccess &createAccess(ArrayRef<Instruction *> Insts,
                             const MemoryRegion &Region, AccessKind Kind);
  void mergeRepresentationFrom(const Observation &Other);
};

class MAIRGraph final {
  friend class MAIRVerifier;
  friend class Observation;

public:
  using ObservationListType =
      simple_ilist<Observation, ilist_parent<MAIRGraph>>;
  using iterator = ObservationListType::iterator;
  using const_iterator = ObservationListType::const_iterator;
  using EdgeListType = SmallVector<Observation *, 2>;
  using ConstEdgeListType = SmallVector<const Observation *, 2>;
  using ObservationEdgeListType = SmallVector<ObservationEdge, 2>;

private:
  const Function *F = nullptr;
  SpecificBumpPtrAllocator<Observation> GraphAllocator;
  SpecificBumpPtrAllocator<MemoryObject> MemoryObjectAllocator;
  std::unique_ptr<MAIRScalarExprOwner> ScalarExprOwner;
  SpecificBumpPtrAllocator<MAIRScalarExprNode> ScalarExprAllocator;
  DenseMap<const Value *, MemoryObject *> MemoryObjects;
  DenseMap<const BasicBlock *, Observation *> BBEntryRepresentatives;
  DenseMap<const BasicBlock *, Observation *> BBExitRepresentatives;
  ObservationListType Observations;
  unsigned NumObservations = 0;
  unsigned NumEdges = 0;
  unsigned NextObservationID = 0;

public:
  MAIRGraph();
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
  /// Create an additional semantic event without changing boundary maps.
  Observation &createObservation(ObservationSite Site,
                                 ObservationGuard Guard = AlwaysGuard{});
  Observation &createEdgeObservation(const BasicBlock &From,
                                     const BasicBlock &To,
                                     ObservationGuard Guard = AlwaysGuard{});

  MAIRScalarExpr createConstantExpr(Type &Ty, const APInt &Value);
  MAIRScalarExpr createValueExpr(const Value &Value);
  MAIRScalarExpr createUnaryExpr(MAIRScalarExpr::Kind Kind, Type &ResultType,
                                 MAIRScalarExpr Operand);
  MAIRScalarExpr createBinaryExpr(MAIRScalarExpr::Kind Kind, Type &ResultType,
                                  MAIRScalarExpr LHS, MAIRScalarExpr RHS);
  bool ownsScalarExpr(MAIRScalarExpr Expr) const;

  iterator erase(Observation &Obs);
  void clear();

  Observation *find(StringRef Name);
  const Observation *find(StringRef Name) const;
  Observation *getBBEntryRepresentative(const BasicBlock &BB);
  const Observation *getBBEntryRepresentative(const BasicBlock &BB) const;
  Observation *getBBExitRepresentative(const BasicBlock &BB);
  const Observation *getBBExitRepresentative(const BasicBlock &BB) const;
  void replaceObservation(Observation &From, Observation &Into);

  void addEdge(Observation &From, Observation &To, ObsEdgeKind Kind);
  void removeEdge(Observation &From, Observation &To);
  bool hasEdge(const Observation &From, const Observation &To) const;
  bool hasEdge(const Observation &From, const Observation &To,
               ObsEdgeKind Kind) const;
  EdgeListType successors(Observation &Obs);
  ConstEdgeListType successors(const Observation &Obs) const;
  ObservationEdgeListType childEdges(const Observation &Obs) const;
  EdgeListType predecessors(Observation &Obs);
  ConstEdgeListType predecessors(const Observation &Obs) const;
  ObservationEdgeListType parentEdges(const Observation &Obs) const;
  MemoryObject *getMemoryObject(const Value &Base);
  const MemoryObject *getMemoryObject(const Value &Base) const;

  bool verify(raw_ostream *OS = nullptr) const;
  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;
  void dump(raw_ostream &OS) const { print(OS); }

private:
  template <class T, class... Args> T &allocateNode(Args &&...ArgsPack);
  std::string createObservationName(StringRef Prefix);
  Observation &addObservation(Observation &Obs);
  MemoryObject &getOrCreateMemoryObject(const Value &Base);
  void registerAccess(MemoryAccess &Access);
  void unregisterAccess(MemoryAccess &Access);

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

namespace llvm {

template <> struct GraphTraits<mair::MemoryAccess *> {
  using NodeRef = mair::MemoryAccess *;
  using ChildIteratorType = mair::MemoryAccess *const *;

  static NodeRef getEntryNode(NodeRef Access) { return Access; }
  static ChildIteratorType child_begin(NodeRef) { return nullptr; }
  static ChildIteratorType child_end(NodeRef) { return nullptr; }
};

template <> struct GraphTraits<const mair::MemoryAccess *> {
  using NodeRef = const mair::MemoryAccess *;
  using ChildIteratorType = const mair::MemoryAccess *const *;

  static NodeRef getEntryNode(NodeRef Access) { return Access; }
  static ChildIteratorType child_begin(NodeRef) { return nullptr; }
  static ChildIteratorType child_end(NodeRef) { return nullptr; }
};

template <> struct GraphTraits<mair::Observation *> {
  using NodeRef = mair::Observation *;
  using ChildIteratorType = mair::ObservationEdgeIterator<mair::Observation>;
  using EdgeRef = mair::ObservationEdge;
  using ChildEdgeIteratorType = ArrayRef<mair::ObservationEdge>::iterator;

  static NodeRef getEntryNode(NodeRef Obs) { return Obs; }
  static ChildIteratorType child_begin(NodeRef Obs) {
    return ChildIteratorType(Obs->children().begin());
  }
  static ChildIteratorType child_end(NodeRef Obs) {
    return ChildIteratorType(Obs->children().end());
  }
  static ChildEdgeIteratorType child_edge_begin(NodeRef Obs) {
    return Obs->children().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef Obs) {
    return Obs->children().end();
  }
  static NodeRef edge_dest(EdgeRef Edge) { return &Edge.getNode(); }
};

template <> struct GraphTraits<const mair::Observation *> {
  using NodeRef = const mair::Observation *;
  using ChildIteratorType =
      mair::ObservationEdgeIterator<const mair::Observation>;
  using EdgeRef = mair::ObservationEdge;
  using ChildEdgeIteratorType = ArrayRef<mair::ObservationEdge>::iterator;

  static NodeRef getEntryNode(NodeRef Obs) { return Obs; }
  static ChildIteratorType child_begin(NodeRef Obs) {
    return ChildIteratorType(Obs->children().begin());
  }
  static ChildIteratorType child_end(NodeRef Obs) {
    return ChildIteratorType(Obs->children().end());
  }
  static ChildEdgeIteratorType child_edge_begin(NodeRef Obs) {
    return Obs->children().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef Obs) {
    return Obs->children().end();
  }
  static NodeRef edge_dest(EdgeRef Edge) { return &Edge.getNode(); }
};

template <> struct GraphTraits<Inverse<mair::Observation *>> {
  using NodeRef = mair::Observation *;
  using ChildIteratorType = mair::ObservationEdgeIterator<mair::Observation>;

  static NodeRef getEntryNode(Inverse<mair::Observation *> G) {
    return G.Graph;
  }
  static ChildIteratorType child_begin(NodeRef Obs) {
    return ChildIteratorType(Obs->parents().begin());
  }
  static ChildIteratorType child_end(NodeRef Obs) {
    return ChildIteratorType(Obs->parents().end());
  }
};

template <> struct GraphTraits<Inverse<const mair::Observation *>> {
  using NodeRef = const mair::Observation *;
  using ChildIteratorType =
      mair::ObservationEdgeIterator<const mair::Observation>;

  static NodeRef getEntryNode(Inverse<const mair::Observation *> G) {
    return G.Graph;
  }
  static ChildIteratorType child_begin(NodeRef Obs) {
    return ChildIteratorType(Obs->parents().begin());
  }
  static ChildIteratorType child_end(NodeRef Obs) {
    return ChildIteratorType(Obs->parents().end());
  }
};

template <>
struct GraphTraits<mair::MAIRGraph *>
    : public GraphTraits<mair::Observation *> {
  using nodes_iterator = pointer_iterator<mair::MAIRGraph::iterator>;

  static NodeRef getEntryNode(mair::MAIRGraph *G) {
    return G && !G->empty() ? &*G->begin() : nullptr;
  }
  static nodes_iterator nodes_begin(mair::MAIRGraph *G) {
    return nodes_iterator(G->begin());
  }
  static nodes_iterator nodes_end(mair::MAIRGraph *G) {
    return nodes_iterator(G->end());
  }
  static size_t size(mair::MAIRGraph *G) { return G->size(); }
};

template <>
struct GraphTraits<const mair::MAIRGraph *>
    : public GraphTraits<const mair::Observation *> {
  using nodes_iterator = pointer_iterator<mair::MAIRGraph::const_iterator>;

  static NodeRef getEntryNode(const mair::MAIRGraph *G) {
    return G && !G->empty() ? &*G->begin() : nullptr;
  }
  static nodes_iterator nodes_begin(const mair::MAIRGraph *G) {
    return nodes_iterator(G->begin());
  }
  static nodes_iterator nodes_end(const mair::MAIRGraph *G) {
    return nodes_iterator(G->end());
  }
  static size_t size(const mair::MAIRGraph *G) { return G->size(); }
};

template <>
struct GraphTraits<Inverse<mair::MAIRGraph *>>
    : public GraphTraits<Inverse<mair::Observation *>> {
  using nodes_iterator = pointer_iterator<mair::MAIRGraph::iterator>;

  static NodeRef getEntryNode(Inverse<mair::MAIRGraph *> G) {
    return G.Graph && !G.Graph->empty() ? &*G.Graph->begin() : nullptr;
  }
  static nodes_iterator nodes_begin(Inverse<mair::MAIRGraph *> G) {
    return nodes_iterator(G.Graph->begin());
  }
  static nodes_iterator nodes_end(Inverse<mair::MAIRGraph *> G) {
    return nodes_iterator(G.Graph->end());
  }
};

template <>
struct GraphTraits<Inverse<const mair::MAIRGraph *>>
    : public GraphTraits<Inverse<const mair::Observation *>> {
  using nodes_iterator = pointer_iterator<mair::MAIRGraph::const_iterator>;

  static NodeRef getEntryNode(Inverse<const mair::MAIRGraph *> G) {
    return G.Graph && !G.Graph->empty() ? &*G.Graph->begin() : nullptr;
  }
  static nodes_iterator nodes_begin(Inverse<const mair::MAIRGraph *> G) {
    return nodes_iterator(G.Graph->begin());
  }
  static nodes_iterator nodes_end(Inverse<const mair::MAIRGraph *> G) {
    return nodes_iterator(G.Graph->end());
  }
};

} // namespace llvm
