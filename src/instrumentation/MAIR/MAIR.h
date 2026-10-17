#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitmaskEnum.h"
#include "llvm/ADT/DenseMap.h"
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
#include "llvm/Analysis/LoopInfo.h"
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
using llvm::LoopInfo;
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
class Observation;
class MemoryObject;
class MemoryUse;
class ProgramPoint;
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
  LLVM_MARK_AS_BITMASK_ENUM(CallLike)
};
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

enum class PPEdgeKind : uint8_t;

/// Sanitizer-specific legality rules shared by every MAIR optimization pass.
/// Region geometry remains a property of Observation; this configuration
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

  bool allowsCoverageAlong(PPEdgeKind Kind) const;
  uint8_t getLevel(AccessKind Kind) const;
  void initializeObservation(Observation &Obs) const;
};

enum class PPEdgeKind : uint8_t { Dom, PostDom };

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

/// A physical placement for one program point. Edge sites remain virtual
/// until instantiate() creates a dedicated CFG block in a later stage.
class ProgramPointSite {
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

  ProgramPointSite() = default;

public:
  static ProgramPointSite anchored(const Instruction &Anchor, Kind K);
  static ProgramPointSite edge(const BasicBlock &From, const BasicBlock &To);

  Kind getKind() const { return SiteKind; }
  bool isAnchored() const { return SiteKind != Kind::Edge; }
  bool isEdge() const { return SiteKind == Kind::Edge; }
  /// Edge sites are anchored at the source terminator, so this returns the
  /// source block rather than the block created later by materialization.
  const BasicBlock *getBasicBlock() const { return getAnchor().getParent(); }
  const Instruction &getAnchor() const {
    assert(Anchor && "program point site has no anchor");
    return *Anchor;
  }
  const BasicBlock *getEdgeFrom() const { return isEdge() ? From : nullptr; }
  const BasicBlock *getEdgeTo() const { return isEdge() ? To : nullptr; }

  bool operator==(const ProgramPointSite &Other) const {
    return SiteKind == Other.SiteKind && Anchor == Other.Anchor &&
           From == Other.From && To == Other.To;
  }
  bool operator!=(const ProgramPointSite &Other) const {
    return !(*this == Other);
  }

  void print(raw_ostream &OS) const;
};

/// Execute the program point observations whenever control reaches its site.
struct AlwaysGuard {
  bool operator==(const AlwaysGuard &) const { return true; }
  bool operator!=(const AlwaysGuard &) const { return false; }
};

/// Execute only if at least one represented source instruction ran. The
/// instruction list has set semantics; storage order is retained for dumps.
class AnyProvenanceExecutedGuard {
  SmallVector<Instruction *, 2> Provenance;
  const BasicBlock *ResetBlock = nullptr;

public:
  AnyProvenanceExecutedGuard() = default;
  explicit AnyProvenanceExecutedGuard(ArrayRef<Instruction *> Instructions)
      : Provenance(Instructions) {}
  AnyProvenanceExecutedGuard(ArrayRef<Instruction *> Instructions,
                             const BasicBlock *Reset)
      : Provenance(Instructions), ResetBlock(Reset) {}

  ArrayRef<Instruction *> getProvenance() const { return Provenance; }
  const BasicBlock *getResetBlock() const { return ResetBlock; }
  bool operator==(const AnyProvenanceExecutedGuard &Other) const;
  bool operator!=(const AnyProvenanceExecutedGuard &Other) const {
    return !(*this == Other);
  }
};

using ProgramPointGuard = std::variant<AlwaysGuard, AnyProvenanceExecutedGuard>;

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

class ProgramPointEdge {
  ProgramPoint *Node = nullptr;
  PPEdgeKind Kind = PPEdgeKind::Dom;

public:
  ProgramPointEdge() = default;
  ProgramPointEdge(ProgramPoint &N, PPEdgeKind K) : Node(&N), Kind(K) {}

  ProgramPoint &getNode() const {
    assert(Node && "MAIR program point edge has no endpoint");
    return *Node;
  }
  PPEdgeKind getKind() const { return Kind; }
  bool isDom() const { return Kind == PPEdgeKind::Dom; }
  bool isPostDom() const { return Kind == PPEdgeKind::PostDom; }
};

/// The operand edge from an Observation user to a MemoryObject, analogous to
/// llvm::Use. It maintains the object's intrusive use-list.
class MemoryUse {
  friend class Observation;
  friend class MAIRGraph;

  MemoryObject *Object = nullptr;
  MemoryUse *Next = nullptr;
  MemoryUse **Prev = nullptr;
  Observation *Parent = nullptr;

public:
  MemoryUse() = default;
  explicit MemoryUse(Observation &P) : Parent(&P) {}
  ~MemoryUse() { set(nullptr); }

  MemoryUse(const MemoryUse &) = delete;
  MemoryUse &operator=(const MemoryUse &) = delete;
  MemoryUse(MemoryUse &&) = delete;
  MemoryUse &operator=(MemoryUse &&) = delete;

  MemoryObject *get() const { return Object; }
  Observation *getUser() const { return Parent; }
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
  Observation &operator()(MemoryUse &Use) const { return *Use.getUser(); }
  const Observation &operator()(const MemoryUse &Use) const {
    return *Use.getUser();
  }
};

/// A graph-owned wrapper for one LLVM base pointer, analogous to llvm::Value.
/// Memory observations referring to the base are reachable through its
/// use-list.
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

template <class NodeT> class ProgramPointEdgeIterator {
  ArrayRef<ProgramPointEdge>::iterator It = nullptr;

public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = NodeT *;
  using difference_type = std::ptrdiff_t;
  using pointer = NodeT **;
  using reference = NodeT *;

  ProgramPointEdgeIterator() = default;
  explicit ProgramPointEdgeIterator(ArrayRef<ProgramPointEdge>::iterator I)
      : It(I) {}

  reference operator*() const { return &It->getNode(); }
  ProgramPointEdgeIterator &operator++() {
    ++It;
    return *this;
  }
  ProgramPointEdgeIterator operator++(int) {
    ProgramPointEdgeIterator Tmp = *this;
    ++*this;
    return Tmp;
  }
  bool operator==(const ProgramPointEdgeIterator &Other) const {
    return It == Other.It;
  }
  bool operator!=(const ProgramPointEdgeIterator &Other) const {
    return !(*this == Other);
  }

  PPEdgeKind getKind() const { return It->getKind(); }
};

/// An identity-bearing memory observation (o), owned by one ProgramPoint.
class Observation final
    : public ilist_node_with_parent<Observation, ProgramPoint,
                                    ilist_parent<ProgramPoint>> {
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
  Observation() = default;
  Observation(ArrayRef<Instruction *> I, AbstractAccess A, AccessKind K,
              AccessProperty P = AccessProperty::None)
      : Observation(I, MemoryRegion(std::move(A)), K, P) {}
  Observation(ArrayRef<Instruction *> I, MemoryRegion R, AccessKind K,
              AccessProperty P = AccessProperty::None)
      : Insts(I.begin(), I.end()), Region(std::move(R)), Kind(K), Properties(P),
        BaseUse(*this) {
    MAIROptimizationConfig::generic().initializeObservation(*this);
  }

  Observation(const Observation &) = delete;
  Observation &operator=(const Observation &) = delete;
  Observation(Observation &&) = delete;
  Observation &operator=(Observation &&) = delete;

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
    assert(BaseUse.get() && "MAIR observation has no memory object");
    return *BaseUse.get();
  }
  const MemoryObject &getMemoryObject() const {
    assert(BaseUse.get() && "MAIR observation has no memory object");
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
    return any(Properties & Property);
  }
  bool isVolatile() const { return hasProperty(AccessProperty::Volatile); }
  bool isAtomic() const { return hasProperty(AccessProperty::Atomic); }
  bool isCallLike() const { return hasProperty(AccessProperty::CallLike); }
  bool isMasked() const { return getMaskedRegion() != nullptr; }

  bool isMergeable() const { return Mergeable; }
  bool isMovable() const { return Movable; }
  bool isDeletable() const { return Deletable; }
  uint8_t getLevel() const { return Level; }

  /// Capabilities and level are fixed by the graph configuration when the
  /// observation is registered. Coverage requires LHS level >= RHS level, while
  /// merging requires equal levels.
  bool covers(const Observation &Other) const;
  bool mergeWith(const Observation &Other);
  void mergeKind(AccessKind Other);
  void eraseFromParent();

  bool hasConcreteRange() const;
  static Observation fromInstruction(const Instruction &I,
                                     const DataLayout &DL);

  std::string toString() const;
  void print(raw_ostream &OS) const;

private:
  friend class MAIRGraph;
  friend class MAIROptimizationConfig;
  friend class ProgramPoint;

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

/// A program point (PP) owns a site, a guard, and a list of observations.
/// Like a BasicBlock containing Instructions, this models containment only.
class ProgramPoint final
    : public ilist_node_with_parent<ProgramPoint, MAIRGraph,
                                    ilist_parent<MAIRGraph>> {
public:
  using SiteKind = ProgramPointSite::Kind;
  using ObservationListType =
      simple_ilist<Observation, ilist_parent<ProgramPoint>>;
  using iterator = ObservationListType::iterator;
  using const_iterator = ObservationListType::const_iterator;
  using ObservationComparator = bool (*)(const Observation &,
                                         const Observation &);
  using EdgeListType = SmallVector<ProgramPointEdge, 4>;

private:
  friend class Observation;

  std::string Name;
  ProgramPointSite Site;
  ProgramPointGuard Guard = AlwaysGuard{};
  SpecificBumpPtrAllocator<Observation> ObservationAllocator;
  ObservationListType Observations;
  EdgeListType Children;
  EdgeListType Parents;
  SmallPtrSet<const BasicBlock *, 2> RepresentedBlocks;
  unsigned NumObservations = 0;
  bool Sorted = true;
  ObservationComparator SortedWith = nullptr;

  void invalidateSort() { Sorted = false; }

public:
  ProgramPoint(StringRef N, const Instruction &Anchor,
               SiteKind K = SiteKind::MemoryInst);
  ProgramPoint(StringRef N, ProgramPointSite S, ProgramPointGuard G);
  ~ProgramPoint();

  ProgramPoint(const ProgramPoint &) = delete;
  ProgramPoint &operator=(const ProgramPoint &) = delete;
  ProgramPoint(ProgramPoint &&) = delete;
  ProgramPoint &operator=(ProgramPoint &&) = delete;

  StringRef getName() const { return Name; }
  const ProgramPointSite &getSite() const { return Site; }
  const ProgramPointGuard &getGuard() const { return Guard; }
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

  const ObservationListType &observations() const { return Observations; }
  ArrayRef<ProgramPointEdge> children() const { return Children; }
  ArrayRef<ProgramPointEdge> parents() const { return Parents; }
  iterator begin() { return Observations.begin(); }
  iterator end() { return Observations.end(); }
  const_iterator begin() const { return Observations.begin(); }
  const_iterator end() const { return Observations.end(); }
  bool empty() const { return NumObservations == 0; }
  unsigned size() const { return NumObservations; }
  bool hasChild(const ProgramPoint &PP, PPEdgeKind Kind) const;
  bool hasParent(const ProgramPoint &PP, PPEdgeKind Kind) const;
  /// Merging preserves execution semantics only for equal guards and equal
  /// anchored kinds, or for program points on the exact same CFG edge.
  bool hasCompatibleMergeSemantics(const ProgramPoint &Other) const;

  Observation &addObservation(const Observation &A);
  /// Normalize observation order. MAIR analyses must not depend on list order.
  void sort(ObservationComparator Compare);
  iterator erase(iterator It);
  iterator erase(Observation &Obs);
  void mergeObservationsFrom(const ProgramPoint &Other);
  void prependObservationsFrom(const ProgramPoint &Other);

  std::string toString() const;
  void print(raw_ostream &OS) const;

private:
  friend class MAIRGraph;
  friend class MAIRVerifier;

  static ObservationListType ProgramPoint::*getSublistAccess(Observation *) {
    return &ProgramPoint::Observations;
  }
  Observation &createObservation(ArrayRef<Instruction *> Insts,
                                 const MemoryRegion &Region, AccessKind Kind,
                                 AccessProperty Properties);
  void mergeRepresentationFrom(const ProgramPoint &Other);
};

class MAIRGraph final {
  friend class MAIRVerifier;
  friend class ProgramPoint;
  friend class SimplifyGraphPass;

public:
  using ProgramPointListType =
      simple_ilist<ProgramPoint, ilist_parent<MAIRGraph>>;
  using iterator = ProgramPointListType::iterator;
  using const_iterator = ProgramPointListType::const_iterator;

private:
  enum class State : uint8_t { Live, ConsumedByInstantiation, MovedFrom };

  struct FunctionAnalysisState {
    std::unique_ptr<DominatorTree> OwnedDT;
    std::unique_ptr<PostDominatorTree> OwnedPDT;
    std::unique_ptr<LoopInfo> OwnedLI;
    const DominatorTree *DT = nullptr;
    const PostDominatorTree *PDT = nullptr;
    const LoopInfo *LI = nullptr;

    void bind(const DominatorTree &NewDT, const PostDominatorTree &NewPDT,
              const LoopInfo &NewLI) {
      DT = &NewDT;
      PDT = &NewPDT;
      LI = &NewLI;
    }
    void takeOwnership(DominatorTree &&NewDT, PostDominatorTree &&NewPDT,
                       LoopInfo &&NewLI) {
      OwnedDT = std::make_unique<DominatorTree>(std::move(NewDT));
      OwnedPDT = std::make_unique<PostDominatorTree>(std::move(NewPDT));
      OwnedLI = std::make_unique<LoopInfo>(std::move(NewLI));
      bind(*OwnedDT, *OwnedPDT, *OwnedLI);
    }
    void clearReferences() { DT = nullptr, PDT = nullptr, LI = nullptr; }
  };

  const Function *F = nullptr;
  Function *MutableF = nullptr;
  State GraphState = State::Live;
  MAIROptimizationConfig OptimizationConfig;
  SpecificBumpPtrAllocator<ProgramPoint> GraphAllocator;
  SpecificBumpPtrAllocator<MemoryObject> MemoryObjectAllocator;
  std::unique_ptr<MAIRScalarExprOwner> ScalarExprOwner;
  SpecificBumpPtrAllocator<MAIRScalarExprNode> ScalarExprAllocator;
  FunctionAnalysisState Analyses;
  DenseMap<const Value *, MemoryObject *> MemoryObjects;
  DenseMap<const BasicBlock *, ProgramPoint *> BBEntryRepresentatives;
  DenseMap<const BasicBlock *, ProgramPoint *> BBExitRepresentatives;
  ProgramPointListType ProgramPoints;
  unsigned NumProgramPoints = 0;
  unsigned NumEdges = 0;
  unsigned NextProgramPointID = 0;

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
    assert(Analyses.DT && "MAIRGraph was constructed without a dominator tree");
    return *Analyses.DT;
  }
  const PostDominatorTree &getPostDominatorTree() const {
    assertLive("query post-dominance analysis");
    assert(Analyses.PDT &&
           "MAIRGraph was constructed without a post-dominator tree");
    return *Analyses.PDT;
  }
  bool hasLoopInfo() const { return Analyses.LI != nullptr; }
  const LoopInfo &getLoopInfo() const {
    assertLive("query loop analysis");
    assert(Analyses.LI && "MAIRGraph was constructed without loop information");
    return *Analyses.LI;
  }
  bool isLive() const { return GraphState == State::Live; }
  bool isConsumed() const {
    return GraphState == State::ConsumedByInstantiation;
  }
  bool empty() const {
    assertLive("query");
    return NumProgramPoints == 0;
  }
  size_t size() const {
    assertLive("query");
    return NumProgramPoints;
  }

  /// Build a MAIR graph for \p F using DT/PDT/LI results owned by \p FAM.
  ///
  /// The graph borrows the exact FAM results. They must remain valid, and the
  /// underlying LLVM IR must not change, throughout the graph's live lifetime.
  template <class BarrierTrait = DefaultBarrierTrait>
  static MAIRGraph
  build(Function &F, FunctionAnalysisManager &FAM,
        MAIROptimizationConfig Config = MAIROptimizationConfig::generic()) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
    auto &LI = FAM.getResult<llvm::LoopAnalysis>(F);
    MAIRBarrierAnalysis<BarrierTrait> BarrierAnalysis;
    return build(F, DT, PDT, LI, BarrierAnalysis, Config);
  }

  /// Build a MAIR graph that takes ownership of caller-provided analyses.
  ///
  /// Passing analyses by rvalue makes the graph lifetime explicit. Callers
  /// that retain analysis ownership should use the FAM overload above.
  template <class BarrierTrait = DefaultBarrierTrait>
  static MAIRGraph
  build(Function &F, DominatorTree &&DT, PostDominatorTree &&PDT, LoopInfo &&LI,
        MAIROptimizationConfig Config = MAIROptimizationConfig::generic()) {
    MAIRBarrierAnalysis<BarrierTrait> BarrierAnalysis;
    MAIRGraph Graph = build(F, DT, PDT, LI, BarrierAnalysis, Config);
    Graph.Analyses.takeOwnership(std::move(DT), std::move(PDT), std::move(LI));
    return Graph;
  }

  iterator begin() {
    assertLive("iterate");
    return ProgramPoints.begin();
  }
  iterator end() {
    assertLive("iterate");
    return ProgramPoints.end();
  }
  const_iterator begin() const {
    assertLive("iterate");
    return ProgramPoints.begin();
  }
  const_iterator end() const {
    assertLive("iterate");
    return ProgramPoints.end();
  }

  ProgramPoint &createBBProgramPoint(const BasicBlock &BB, bool IsEntry);
  ProgramPoint &createInstProgramPoint(const BasicBlock &BB,
                                       const Instruction &Anchor);
  /// Create an additional semantic event without changing boundary maps.
  ProgramPoint &createProgramPoint(ProgramPointSite Site,
                                   ProgramPointGuard Guard = AlwaysGuard{});
  ProgramPoint &createEdgeProgramPoint(const BasicBlock &From,
                                       const BasicBlock &To,
                                       ProgramPointGuard Guard = AlwaysGuard{});

  MAIRScalarExpr createConstantExpr(Type &Ty, const APInt &Value);
  MAIRScalarExpr createValueExpr(const Value &Value);
  MAIRScalarExpr createUnaryExpr(MAIRScalarExpr::Kind Kind, Type &ResultType,
                                 MAIRScalarExpr Operand);
  MAIRScalarExpr createBinaryExpr(MAIRScalarExpr::Kind Kind, Type &ResultType,
                                  MAIRScalarExpr LHS, MAIRScalarExpr RHS);
  bool ownsScalarExpr(MAIRScalarExpr Expr) const;

  iterator erase(ProgramPoint &PP);
  void clear();

  ProgramPoint *find(StringRef Name);
  const ProgramPoint *find(StringRef Name) const;
  ProgramPoint &getBBEntryRepresentative(const BasicBlock &BB);
  const ProgramPoint &getBBEntryRepresentative(const BasicBlock &BB) const;
  ProgramPoint &getBBExitRepresentative(const BasicBlock &BB);
  const ProgramPoint &getBBExitRepresentative(const BasicBlock &BB) const;
  void replaceProgramPoint(ProgramPoint &From, ProgramPoint &Into);

  void addEdge(ProgramPoint &From, ProgramPoint &To, PPEdgeKind Kind);
  void removeEdge(ProgramPoint &From, ProgramPoint &To, PPEdgeKind Kind);
  bool hasEdge(const ProgramPoint &From, const ProgramPoint &To,
               PPEdgeKind Kind) const;
  /// Return whether \p Ancestor reaches \p Descendant through one or more
  /// program point edges of exactly \p Kind.
  bool isAncestor(const ProgramPoint &Ancestor, const ProgramPoint &Descendant,
                  PPEdgeKind Kind) const;
  MemoryObject *getMemoryObject(const Value &Base);
  const MemoryObject *getMemoryObject(const Value &Base) const;

  bool verify(raw_ostream *OS = nullptr) const;
  std::string toString() const;
  void print(raw_ostream &OS) const;
  void dump(raw_ostream &OS) const { print(OS); }

  llvm::Expected<MAIRInstantiationResult>
  instantiate(llvm::FunctionAnalysisManager &FAM,
              MAIRObservationBackend &Backend);

private:
  void assertLive(StringRef Operation) const;
  void markConsumed() { GraphState = State::ConsumedByInstantiation; }
  void clearStorage();

  template <class T, class... Args> T &allocateNode(Args &&...ArgsPack);
  std::string createProgramPointName();
  ProgramPoint &addProgramPoint(ProgramPoint &PP);
  MemoryObject &getOrCreateMemoryObject(const Value &Base);
  void registerObservation(Observation &Obs);
  void unregisterObservation(Observation &Obs);
  void contractProgramPoint(ProgramPoint &Into, ProgramPoint &From);

  /// Construct the raw MAIR graph once dominance and barrier analyses are
  /// fixed.
  ///
  /// Creates BB-entry, memory-instruction, and BB-exit program points, then
  /// adds intra-block and dominance/post-dominance edges gated by \p
  /// BarrierAnalysis.
  static MAIRGraph build(Function &F, const DominatorTree &DT,
                         const PostDominatorTree &PDT, const LoopInfo &LI,
                         MAIRBarrierAnalysisCore &BarrierAnalysis,
                         MAIROptimizationConfig Config);

  static ProgramPointListType MAIRGraph::*getSublistAccess(ProgramPoint *) {
    return &MAIRGraph::ProgramPoints;
  }
};

} // namespace mair

namespace llvm {

template <> struct GraphTraits<mair::Observation *> {
  using NodeRef = mair::Observation *;
  using ChildIteratorType = mair::Observation *const *;

  static NodeRef getEntryNode(NodeRef Obs) { return Obs; }
  static ChildIteratorType child_begin(NodeRef) { return nullptr; }
  static ChildIteratorType child_end(NodeRef) { return nullptr; }
};

template <> struct GraphTraits<const mair::Observation *> {
  using NodeRef = const mair::Observation *;
  using ChildIteratorType = const mair::Observation *const *;

  static NodeRef getEntryNode(NodeRef Obs) { return Obs; }
  static ChildIteratorType child_begin(NodeRef) { return nullptr; }
  static ChildIteratorType child_end(NodeRef) { return nullptr; }
};

template <> struct GraphTraits<mair::ProgramPoint *> {
  using NodeRef = mair::ProgramPoint *;
  using ChildIteratorType = mair::ProgramPointEdgeIterator<mair::ProgramPoint>;
  using EdgeRef = mair::ProgramPointEdge;
  using ChildEdgeIteratorType = ArrayRef<mair::ProgramPointEdge>::iterator;

  static NodeRef getEntryNode(NodeRef PP) { return PP; }
  static ChildIteratorType child_begin(NodeRef PP) {
    return ChildIteratorType(PP->children().begin());
  }
  static ChildIteratorType child_end(NodeRef PP) {
    return ChildIteratorType(PP->children().end());
  }
  static ChildEdgeIteratorType child_edge_begin(NodeRef PP) {
    return PP->children().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef PP) {
    return PP->children().end();
  }
  static NodeRef edge_dest(EdgeRef Edge) { return &Edge.getNode(); }
};

template <> struct GraphTraits<const mair::ProgramPoint *> {
  using NodeRef = const mair::ProgramPoint *;
  using ChildIteratorType =
      mair::ProgramPointEdgeIterator<const mair::ProgramPoint>;
  using EdgeRef = mair::ProgramPointEdge;
  using ChildEdgeIteratorType = ArrayRef<mair::ProgramPointEdge>::iterator;

  static NodeRef getEntryNode(NodeRef PP) { return PP; }
  static ChildIteratorType child_begin(NodeRef PP) {
    return ChildIteratorType(PP->children().begin());
  }
  static ChildIteratorType child_end(NodeRef PP) {
    return ChildIteratorType(PP->children().end());
  }
  static ChildEdgeIteratorType child_edge_begin(NodeRef PP) {
    return PP->children().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef PP) {
    return PP->children().end();
  }
  static NodeRef edge_dest(EdgeRef Edge) { return &Edge.getNode(); }
};

template <> struct GraphTraits<Inverse<mair::ProgramPoint *>> {
  using NodeRef = mair::ProgramPoint *;
  using ChildIteratorType = mair::ProgramPointEdgeIterator<mair::ProgramPoint>;

  static NodeRef getEntryNode(Inverse<mair::ProgramPoint *> G) {
    return G.Graph;
  }
  static ChildIteratorType child_begin(NodeRef PP) {
    return ChildIteratorType(PP->parents().begin());
  }
  static ChildIteratorType child_end(NodeRef PP) {
    return ChildIteratorType(PP->parents().end());
  }
};

template <> struct GraphTraits<Inverse<const mair::ProgramPoint *>> {
  using NodeRef = const mair::ProgramPoint *;
  using ChildIteratorType =
      mair::ProgramPointEdgeIterator<const mair::ProgramPoint>;

  static NodeRef getEntryNode(Inverse<const mair::ProgramPoint *> G) {
    return G.Graph;
  }
  static ChildIteratorType child_begin(NodeRef PP) {
    return ChildIteratorType(PP->parents().begin());
  }
  static ChildIteratorType child_end(NodeRef PP) {
    return ChildIteratorType(PP->parents().end());
  }
};

template <>
struct GraphTraits<mair::MAIRGraph *>
    : public GraphTraits<mair::ProgramPoint *> {
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
    : public GraphTraits<const mair::ProgramPoint *> {
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
    : public GraphTraits<Inverse<mair::ProgramPoint *>> {
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
    : public GraphTraits<Inverse<const mair::ProgramPoint *>> {
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
