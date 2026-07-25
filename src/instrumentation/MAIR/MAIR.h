#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
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
#include "llvm/Support/Error.h"
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
#include <utility>
#include <variant>

namespace mair {

using llvm::APInt;
using llvm::ArrayRef;
using llvm::BasicBlock;
using llvm::cast;
using llvm::DataLayout;
using llvm::DenseMap;
using llvm::DenseSet;
using llvm::DominatorTree;
using llvm::DominatorTreeAnalysis;
using llvm::Error;
using llvm::Expected;
using llvm::Function;
using llvm::FunctionAnalysisManager;
using llvm::ilist_node_with_parent;
using llvm::ilist_parent;
using llvm::Instruction;
using llvm::iterator_range;
using llvm::make_range;
using llvm::mapped_iterator;
using llvm::PassManager;
using llvm::PostDominatorTree;
using llvm::PostDominatorTreeAnalysis;
using llvm::raw_ostream;
using llvm::raw_string_ostream;
using llvm::simple_ilist;
using llvm::SmallBitVector;
using llvm::SmallPtrSet;
using llvm::SmallVector;
using llvm::SpecificBumpPtrAllocator;
using llvm::StringRef;
using llvm::Type;
using llvm::Value;

class MAIRGraph;
class MAIRVerifier;
class MAIRObservationBackend;
class SimplifyGraphPass;
class MemoryAccess;
class MemoryObject;
class MemoryUse;
class Observation;
class MAIRScalarExpr;
struct MAIRScalarExprNode;
struct MAIRScalarExprOwner;
struct MAIRInstantiationResult;

enum class AccessKind : uint8_t { Read = 1, Write = 2, ReadWrite = 3 };
/// Orthogonal source semantics. CallLike denotes an access that requires
/// independent call-specific handling, currently a modeled memory intrinsic;
/// it does not classify every access whose LLVM instruction is a CallBase.
enum class AccessProperty : uint8_t {
  None = 0,
  Volatile = 1 << 0,
  Atomic = 1 << 1,
  CallLike = 1 << 2,
};

constexpr AccessProperty operator|(AccessProperty LHS, AccessProperty RHS) {
  return static_cast<AccessProperty>(static_cast<uint8_t>(LHS) |
                                     static_cast<uint8_t>(RHS));
}

constexpr AccessProperty &operator|=(AccessProperty &LHS, AccessProperty RHS) {
  LHS = LHS | RHS;
  return LHS;
}

enum class ObsEdgeKind : uint8_t;

/// Sanitizer-specific legality rules shared by every MAIR optimization pass.
/// Region geometry remains a property of MemoryAccess; this configuration
/// controls which semantic distinctions an observer must preserve.
class MAIROptimizationConfig {
  uint8_t ReadLevel = 1;
  uint8_t WriteLevel = 2;
  uint8_t ReadWriteLevel = 3;
  bool IgnoreAtomic = false;
  bool DomCoverageOnly = false;
  bool AllowRelocation = true;

  explicit MAIROptimizationConfig(uint8_t ReadLevel, uint8_t WriteLevel,
                                  uint8_t ReadWriteLevel, bool IgnoreAtomic,
                                  bool DomCoverageOnly, bool AllowRelocation)
      : ReadLevel(ReadLevel), WriteLevel(WriteLevel),
        ReadWriteLevel(ReadWriteLevel), IgnoreAtomic(IgnoreAtomic),
        DomCoverageOnly(DomCoverageOnly), AllowRelocation(AllowRelocation) {}

public:
  MAIROptimizationConfig() = default;

  static MAIROptimizationConfig generic();
  static MAIROptimizationConfig forAddressSanitizer();
  static MAIROptimizationConfig forThreadSanitizer();

  bool allowsCoverageAlong(ObsEdgeKind Kind) const;
  uint8_t getLevel(AccessKind Kind) const;
  void initializeAccess(MemoryAccess &Access) const;
};

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
                                             const AbstractAccess &B,
                                             const DataLayout *DL = nullptr);

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
  const Instruction *Anchor = nullptr;
  const BasicBlock *From = nullptr;
  const BasicBlock *To = nullptr;

  ObservationSite() = default;

public:
  static ObservationSite anchored(const Instruction &Anchor, Kind K);
  static ObservationSite edge(const BasicBlock &From, const BasicBlock &To);

  Kind getKind() const { return SiteKind; }
  bool isAnchored() const { return SiteKind != Kind::Edge; }
  bool isEdge() const { return SiteKind == Kind::Edge; }
  const BasicBlock *getBasicBlock() const { return getAnchor().getParent(); }
  const Instruction &getAnchor() const {
    assert(Anchor && "observation site has no anchor");
    return *Anchor;
  }
  const BasicBlock *getEdgeFrom() const { return isEdge() ? From : nullptr; }
  const BasicBlock *getEdgeTo() const { return isEdge() ? To : nullptr; }

  bool operator==(const ObservationSite &Other) const {
    return SiteKind == Other.SiteKind && Anchor == Other.Anchor &&
           From == Other.From && To == Other.To;
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

/// A fixed-width vector footprint whose active lanes are selected by a mask.
/// Constant masks are normalized into bits; dynamic masks retain their SSA
/// value and are comparable only when that identity is shared.
class MaskedRegion {
  AbstractAccess Bounds;
  const Value *DynamicMask = nullptr;
  SmallBitVector ConstantMask;
  uint64_t ElementSize = 0;
  bool IsConstant = false;
  bool IsComparable = true;

public:
  MaskedRegion() = default;
  MaskedRegion(AbstractAccess B, const Value &Mask, uint64_t ElementBytes,
               bool Comparable = true);
  MaskedRegion(AbstractAccess B, SmallBitVector Mask, uint64_t ElementBytes);

  const Value *getBase() const { return Bounds.getBase(); }
  const AbstractAccess &getBounds() const { return Bounds; }
  const Value *getDynamicMask() const { return DynamicMask; }
  const SmallBitVector &getConstantMask() const { return ConstantMask; }
  uint64_t getElementSize() const { return ElementSize; }
  unsigned getNumLanes() const;
  bool isConstantMask() const { return IsConstant; }
  bool isComparable() const { return IsComparable; }

  bool contains(const MaskedRegion &Other) const;
  static std::optional<MaskedRegion> merge(const MaskedRegion &A,
                                           const MaskedRegion &B);

  bool operator==(const MaskedRegion &Other) const;
  bool operator!=(const MaskedRegion &Other) const { return !(*this == Other); }
  void print(raw_ostream &OS) const;
};

/// A contiguous, masked, or explicitly periodic memory-access region.
using MemoryRegion = std::variant<AbstractAccess, MaskedRegion, PeriodicRegion>;

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
  AccessProperty Properties = AccessProperty::None;
  bool Mergeable = true;
  bool Movable = true;
  bool Deletable = true;
  uint8_t Level = 1;
  /// Fixed base operand, analogous to a single llvm::User operand.
  MemoryUse BaseUse;

public:
  MemoryAccess() = default;
  MemoryAccess(ArrayRef<Instruction *> I, AbstractAccess A, AccessKind K,
               AccessProperty P = AccessProperty::None)
      : MemoryAccess(I, MemoryRegion(std::move(A)), K, P) {}
  MemoryAccess(ArrayRef<Instruction *> I, MemoryRegion R, AccessKind K,
               AccessProperty P = AccessProperty::None)
      : Insts(I.begin(), I.end()), Region(std::move(R)), Kind(K), Properties(P),
        BaseUse(*this) {
    MAIROptimizationConfig::generic().initializeAccess(*this);
  }

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
  const MaskedRegion *getMaskedRegion() const {
    return std::get_if<MaskedRegion>(&Region);
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
  AccessProperty getProperties() const { return Properties; }
  bool hasProperty(AccessProperty Property) const {
    return (static_cast<uint8_t>(Properties) &
            static_cast<uint8_t>(Property)) != 0;
  }
  bool isVolatile() const { return hasProperty(AccessProperty::Volatile); }
  bool isAtomic() const { return hasProperty(AccessProperty::Atomic); }
  bool isCallLike() const { return hasProperty(AccessProperty::CallLike); }
  bool isMasked() const { return getMaskedRegion() != nullptr; }

  bool isMergeable() const { return Mergeable; }
  bool isMovable() const { return Movable; }
  bool isDeletable() const { return Deletable; }
  uint8_t getLevel() const { return Level; }

  bool covers(const MemoryAccess &Other) const;
  bool mergeWith(const MemoryAccess &Other);
  void mergeKind(AccessKind Other);
  void eraseFromParent();

  bool hasConcreteRange() const;
  static MemoryAccess fromInstruction(const Instruction &I,
                                      const DataLayout &DL);

  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;

private:
  friend class MAIRGraph;
  friend class MAIROptimizationConfig;
  friend class Observation;

  void setOptimizationProperties(bool CanMerge, bool CanMove, bool CanDelete,
                                 uint8_t AccessLevel) {
    Mergeable = CanMerge;
    Movable = CanMove;
    Deletable = CanDelete;
    Level = AccessLevel;
  }
  void refreshOptimizationProperties();
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
  using AccessComparator = bool (*)(const MemoryAccess &, const MemoryAccess &);
  using EdgeListType = SmallVector<ObservationEdge, 4>;

private:
  friend class MemoryAccess;

  std::string Name;
  ObservationSite Site;
  ObservationGuard Guard = AlwaysGuard{};
  SpecificBumpPtrAllocator<MemoryAccess> AccessAllocator;
  AccessListType Accesses;
  EdgeListType Children;
  EdgeListType Parents;
  SmallPtrSet<const BasicBlock *, 2> RepresentedBlocks;
  unsigned NumAccesses = 0;
  bool Sorted = true;
  AccessComparator SortedWith = nullptr;

  void invalidateSort() { Sorted = false; }

public:
  Observation(StringRef N, const Instruction &Anchor,
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
  const Instruction &getAnchor() const { return Site.getAnchor(); }
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
  ArrayRef<ObservationEdge> children() const { return Children; }
  ArrayRef<ObservationEdge> parents() const { return Parents; }
  iterator begin() { return Accesses.begin(); }
  iterator end() { return Accesses.end(); }
  const_iterator begin() const { return Accesses.begin(); }
  const_iterator end() const { return Accesses.end(); }
  bool empty() const { return NumAccesses == 0; }
  unsigned size() const { return NumAccesses; }
  bool hasChild(const Observation &Obs, ObsEdgeKind Kind) const;
  bool hasParent(const Observation &Obs, ObsEdgeKind Kind) const;
  /// Merging preserves execution semantics only for equal guards and equal
  /// anchored kinds, or for observations on the exact same CFG edge.
  bool hasCompatibleMergeSemantics(const Observation &Other) const;

  MemoryAccess &addAccess(const MemoryAccess &A);
  void sort(AccessComparator Compare);
  iterator erase(iterator It);
  iterator erase(MemoryAccess &Access);
  void mergeAccessesFrom(const Observation &Other);
  void prependAccessesFrom(const Observation &Other);

  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;

private:
  friend class MAIRGraph;
  friend class MAIRVerifier;

  static AccessListType Observation::*getSublistAccess(MemoryAccess *) {
    return &Observation::Accesses;
  }
  MemoryAccess &createAccess(ArrayRef<Instruction *> Insts,
                             const MemoryRegion &Region, AccessKind Kind,
                             AccessProperty Properties);
  void mergeRepresentationFrom(const Observation &Other);
};

class MAIRGraph final {
  friend class MAIRVerifier;
  friend class Observation;
  friend class SimplifyGraphPass;

public:
  using ObservationListType =
      simple_ilist<Observation, ilist_parent<MAIRGraph>>;
  using iterator = ObservationListType::iterator;
  using const_iterator = ObservationListType::const_iterator;
  using EdgeListType = SmallVector<Observation *, 2>;
  using ConstEdgeListType = SmallVector<const Observation *, 2>;
  using ObservationEdgeListType = SmallVector<ObservationEdge, 2>;

private:
  enum class State : uint8_t { Live, ConsumedByInstantiation, MovedFrom };

  const Function *F = nullptr;
  Function *MutableF = nullptr;
  State GraphState = State::Live;
  MAIROptimizationConfig OptimizationConfig;
  SpecificBumpPtrAllocator<Observation> GraphAllocator;
  SpecificBumpPtrAllocator<MemoryObject> MemoryObjectAllocator;
  std::unique_ptr<MAIRScalarExprOwner> ScalarExprOwner;
  SpecificBumpPtrAllocator<MAIRScalarExprNode> ScalarExprAllocator;
  std::unique_ptr<DominatorTree> OwnedDT;
  std::unique_ptr<PostDominatorTree> OwnedPDT;
  const DominatorTree *DT = nullptr;
  const PostDominatorTree *PDT = nullptr;
  DenseMap<const Value *, MemoryObject *> MemoryObjects;
  DenseMap<const BasicBlock *, Observation *> BBEntryRepresentatives;
  DenseMap<const BasicBlock *, Observation *> BBExitRepresentatives;
  DenseSet<const BasicBlock *> FwdBarrierBlocks;
  DenseSet<const BasicBlock *> BwdBarrierBlocks;
  ObservationListType Observations;
  unsigned NumObservations = 0;
  unsigned NumEdges = 0;
  unsigned NextObservationID = 0;

public:
  explicit MAIRGraph(
      MAIROptimizationConfig Config = MAIROptimizationConfig::generic());
  explicit MAIRGraph(
      const Function &Func,
      MAIROptimizationConfig Config = MAIROptimizationConfig::generic());
  explicit MAIRGraph(Function &Func, MAIROptimizationConfig Config =
                                         MAIROptimizationConfig::generic());
  MAIRGraph(const MAIRGraph &) = delete;
  MAIRGraph &operator=(const MAIRGraph &) = delete;
  MAIRGraph(MAIRGraph &&Other) noexcept;
  MAIRGraph &operator=(MAIRGraph &&Other) noexcept;
  ~MAIRGraph();

  const Function *getFunction() const { return F; }
  const MAIROptimizationConfig &getOptimizationConfig() const {
    return OptimizationConfig;
  }
  Function *getMutableFunction() { return MutableF; }
  const DominatorTree &getDominatorTree() const {
    assertLive("query dominance analysis");
    assert(DT && "MAIRGraph was constructed without a dominator tree");
    return *DT;
  }
  const PostDominatorTree &getPostDominatorTree() const {
    assertLive("query post-dominance analysis");
    assert(PDT && "MAIRGraph was constructed without a post-dominator tree");
    return *PDT;
  }
  bool isLive() const { return GraphState == State::Live; }
  bool isConsumed() const {
    return GraphState == State::ConsumedByInstantiation;
  }
  bool empty() const {
    assertLive("query");
    return NumObservations == 0;
  }
  size_t size() const {
    assertLive("query");
    return NumObservations;
  }

  /// Build a MAIR graph for \p F using DT/PDT results owned by \p FAM.
  ///
  /// This overload is the pass-pipeline entry point: it must only fetch
  /// reusable LLVM analyses from FAM and bind them to the graph. It
  /// intentionally does not construct DT/PDT itself.
  template <class BarrierTrait = DefaultBarrierTrait>
  static MAIRGraph
  build(Function &F, FunctionAnalysisManager &FAM,
        MAIROptimizationConfig Config = MAIROptimizationConfig::generic()) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
    MAIRBarrierAnalysis<BarrierTrait> BarrierAnalysis;
    return build(F, DT, PDT, BarrierAnalysis, Config);
  }

  /// Build a MAIR graph that takes ownership of caller-provided analyses.
  ///
  /// Passing analyses by rvalue makes the graph lifetime explicit. Callers
  /// that retain analysis ownership should use the FAM overload above.
  template <class BarrierTrait = DefaultBarrierTrait>
  static MAIRGraph
  build(Function &F, DominatorTree &&DT, PostDominatorTree &&PDT,
        MAIROptimizationConfig Config = MAIROptimizationConfig::generic()) {
    MAIRBarrierAnalysis<BarrierTrait> BarrierAnalysis;
    MAIRGraph Graph = build(F, DT, PDT, BarrierAnalysis, Config);
    Graph.OwnedDT = std::make_unique<DominatorTree>(std::move(DT));
    Graph.OwnedPDT = std::make_unique<PostDominatorTree>(std::move(PDT));
    Graph.DT = Graph.OwnedDT.get();
    Graph.PDT = Graph.OwnedPDT.get();
    return Graph;
  }

  iterator begin() {
    assertLive("iterate");
    return Observations.begin();
  }
  iterator end() {
    assertLive("iterate");
    return Observations.end();
  }
  const_iterator begin() const {
    assertLive("iterate");
    return Observations.begin();
  }
  const_iterator end() const {
    assertLive("iterate");
    return Observations.end();
  }

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
  Observation &getBBEntryRepresentative(const BasicBlock &BB);
  const Observation &getBBEntryRepresentative(const BasicBlock &BB) const;
  Observation &getBBExitRepresentative(const BasicBlock &BB);
  const Observation &getBBExitRepresentative(const BasicBlock &BB) const;
  void replaceObservation(Observation &From, Observation &Into);

  void addEdge(Observation &From, Observation &To, ObsEdgeKind Kind);
  void removeEdge(Observation &From, Observation &To);
  void removeEdge(Observation &From, Observation &To, ObsEdgeKind Kind);
  bool hasEdge(const Observation &From, const Observation &To) const;
  bool hasEdge(const Observation &From, const Observation &To,
               ObsEdgeKind Kind) const;
  /// Return whether \p Ancestor reaches \p Descendant through one or more
  /// observation edges of exactly \p Kind.
  bool isAncestor(const Observation &Ancestor, const Observation &Descendant,
                  ObsEdgeKind Kind) const;
  EdgeListType successors(Observation &Obs);
  ConstEdgeListType successors(const Observation &Obs) const;
  ObservationEdgeListType childEdges(const Observation &Obs) const;
  EdgeListType predecessors(Observation &Obs);
  ConstEdgeListType predecessors(const Observation &Obs) const;
  ObservationEdgeListType parentEdges(const Observation &Obs) const;
  MemoryObject *getMemoryObject(const Value &Base);
  const MemoryObject *getMemoryObject(const Value &Base) const;
  bool hasFwdBarrier(const BasicBlock &BB) const {
    return FwdBarrierBlocks.contains(&BB);
  }
  bool hasBwdBarrier(const BasicBlock &BB) const {
    return BwdBarrierBlocks.contains(&BB);
  }

  bool verify(raw_ostream *OS = nullptr) const;
  std::string toString(bool LLVM = false) const;
  void print(raw_ostream &OS, bool LLVM = false) const;
  void dump(raw_ostream &OS) const { print(OS); }

  llvm::Expected<MAIRInstantiationResult>
  instantiate(llvm::FunctionAnalysisManager &FAM,
              MAIRObservationBackend &Backend);

private:
  void assertLive(StringRef Operation) const;
  void markConsumed() { GraphState = State::ConsumedByInstantiation; }
  void clearStorage();

  template <class T, class... Args> T &allocateNode(Args &&...ArgsPack);
  std::string createObservationName(StringRef Prefix);
  Observation &addObservation(Observation &Obs);
  MemoryObject &getOrCreateMemoryObject(const Value &Base);
  void registerAccess(MemoryAccess &Access);
  void unregisterAccess(MemoryAccess &Access);
  void contractObservation(Observation &Into, Observation &From);

  /// Construct the raw MAIR graph once dominance and barrier analyses are
  /// fixed.
  ///
  /// Creates BB-entry, memory-instruction, and BB-exit observations, then adds
  /// intra-block and dominance/post-dominance edges gated by \p
  /// BarrierAnalysis.
  static MAIRGraph build(Function &F, const DominatorTree &DT,
                         const PostDominatorTree &PDT,
                         MAIRBarrierAnalysisCore &BarrierAnalysis,
                         MAIROptimizationConfig Config);

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
