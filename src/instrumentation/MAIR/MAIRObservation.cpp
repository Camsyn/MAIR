#include "MAIR/MAIR.h"

#include "MAIR/MAIRInternal.h"

#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

namespace {
static AccessKind mergeKind(AccessKind LHS, AccessKind RHS) {
  if (LHS == RHS)
    return LHS;
  return AccessKind::ReadWrite;
}

static AbstractValue getStoreSize(Type &Ty, const DataLayout &DL) {
  TypeSize Size = DL.getTypeStoreSize(&Ty);
  if (Size.isScalable())
    return AbstractValue::unknown();
  return AbstractValue::constant(Size.getFixedValue());
}

static std::optional<SmallBitVector> getConstantMask(const Value &Mask) {
  const auto *MaskType = dyn_cast<FixedVectorType>(Mask.getType());
  const auto *MaskConstant = dyn_cast<Constant>(&Mask);
  if (!MaskType || !MaskConstant)
    return std::nullopt;

  SmallBitVector Bits(MaskType->getNumElements());
  for (unsigned Lane = 0; Lane < MaskType->getNumElements(); ++Lane) {
    Constant *Element = MaskConstant->getAggregateElement(Lane);
    const auto *Bit = dyn_cast_or_null<ConstantInt>(Element);
    if (!Bit)
      return std::nullopt;
    if (!Bit->isZero())
      Bits.set(Lane);
  }
  return Bits;
}

static AccessProperty getInstructionProperties(const Instruction &I) {
  AccessProperty Properties = AccessProperty::None;
  if (I.isAtomic())
    Properties |= AccessProperty::Atomic;
  if (const auto *LI = dyn_cast<LoadInst>(&I)) {
    if (LI->isVolatile())
      Properties |= AccessProperty::Volatile;
  } else if (const auto *SI = dyn_cast<StoreInst>(&I)) {
    if (SI->isVolatile())
      Properties |= AccessProperty::Volatile;
  } else if (const auto *MI = dyn_cast<AnyMemIntrinsic>(&I)) {
    Properties |= AccessProperty::CallLike;
    if (MI->isVolatile())
      Properties |= AccessProperty::Volatile;
    if (MI->isAtomic())
      Properties |= AccessProperty::Atomic;
  }
  return Properties;
}

static StringRef getAccessKindName(AccessKind Kind) {
  switch (Kind) {
  case AccessKind::Read:
    return "read";
  case AccessKind::Write:
    return "write";
  case AccessKind::ReadWrite:
    return "read/write";
  }
  llvm_unreachable("unknown access kind");
}

} // namespace

bool isMaskedMemoryIntrinsic(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  switch (II->getIntrinsicID()) {
  case Intrinsic::masked_load:
  case Intrinsic::masked_store:
    return true;
  default:
    return false;
  }
}

AbstractValue getAccessSize(const Value &Value) {
  if (const auto *Size = dyn_cast<ConstantInt>(&Value))
    if (std::optional<uint64_t> Bytes = getNonNegativeU64(Size->getValue()))
      return AbstractValue::constant(*Bytes);
  return AbstractValue::symbol(Value);
}

Observation makeObservation(const Instruction &I, const Value &Ptr,
                            AbstractValue Size, AccessKind Kind,
                            const DataLayout &DL) {
  AbstractAccess Obs =
      AbstractAccess::fromPointerAndSize(Ptr, std::move(Size), DL);
  Instruction *Inst = const_cast<Instruction *>(&I);
  return Observation(ArrayRef<Instruction *>(&Inst, 1), std::move(Obs), Kind,
                     getInstructionProperties(I));
}

void MemoryUse::set(MemoryObject *NewObject) {
  if (Object == NewObject)
    return;
  removeFromList();
  Object = NewObject;
  if (Object)
    addToList(&Object->UseList);
}

void MemoryUse::addToList(MemoryUse **List) {
  assert(!Prev && !Next && "MAIR memory use is already linked");
  Next = *List;
  if (Next)
    Next->Prev = &Next;
  Prev = List;
  *Prev = this;
}

void MemoryUse::removeFromList() {
  if (!Prev)
    return;
  *Prev = Next;
  if (Next)
    Next->Prev = Prev;
  Object = nullptr;
  Next = nullptr;
  Prev = nullptr;
}

MAIROptimizationConfig MAIROptimizationConfig::generic() {
  return MAIROptimizationConfig(/*ReadLevel=*/1, /*WriteLevel=*/2,
                                /*ReadWriteLevel=*/3,
                                /*IgnoreAtomic=*/false,
                                /*DomCoverageOnly=*/false,
                                /*AllowRelocation=*/true);
}

MAIROptimizationConfig MAIROptimizationConfig::forAddressSanitizer() {
  return MAIROptimizationConfig(/*ReadLevel=*/1, /*WriteLevel=*/1,
                                /*ReadWriteLevel=*/1,
                                /*IgnoreAtomic=*/true,
                                /*DomCoverageOnly=*/true,
                                /*AllowRelocation=*/true);
}

MAIROptimizationConfig MAIROptimizationConfig::forThreadSanitizer() {
  return MAIROptimizationConfig(/*ReadLevel=*/1, /*WriteLevel=*/2,
                                /*ReadWriteLevel=*/3,
                                /*IgnoreAtomic=*/false,
                                /*DomCoverageOnly=*/false,
                                /*AllowRelocation=*/true);
}

bool MAIROptimizationConfig::allowsCoverageAlong(PPEdgeKind Kind) const {
  return !DomCoverageOnly || Kind == PPEdgeKind::Dom;
}

uint8_t MAIROptimizationConfig::getLevel(AccessKind Kind) const {
  switch (Kind) {
  case AccessKind::Read:
    return ReadLevel;
  case AccessKind::Write:
    return WriteLevel;
  case AccessKind::ReadWrite:
    return ReadWriteLevel;
  }
  llvm_unreachable("unknown access kind");
}

/// TODO: let this be configurable.
void MAIROptimizationConfig::initializeObservation(Observation &Obs) const {
  const MaskedRegion *Masked = Obs.getMaskedRegion();
  const bool IsComparableMasked = !Masked || Masked->isComparable();
  // TODO: CallLike observations, currently modeled memory intrinsics, should
  // become movable and mergeable. They remain pinned until that is proven.
  const bool CanMerge = !Obs.isVolatile() && !Obs.isCallLike() &&
                        IsComparableMasked && (IgnoreAtomic || !Obs.isAtomic());
  const bool CanMove = AllowRelocation &&
                       // "MA is immovable" does not mean its observation is
                       // immovable.
                       // !Obs.isVolatile() &&
                       !Obs.isCallLike() && !Obs.isAtomic();
  const bool CanDelete = !Obs.isVolatile() && !Obs.isCallLike() &&
                         (IgnoreAtomic || !Obs.isAtomic());
  Obs.setOptimizationProperties(CanMerge, CanMove, CanDelete,
                                getLevel(Obs.getKind()));
}

void Observation::refreshOptimizationProperties() {
  if (ProgramPoint *PP = getParent())
    if (MAIRGraph *Graph = PP->getParent()) {
      Graph->getOptimizationConfig().initializeObservation(*this);
      if (Graph->isUnsupportedLoopBlock(PP->getBasicBlock()))
        setOptimizationProperties(/*CanMerge=*/false, /*CanMove=*/false,
                                  /*CanDelete=*/false, getLevel());
      return;
    }
  MAIROptimizationConfig::generic().initializeObservation(*this);
}

void Observation::mergeKind(AccessKind Other) {
  Kind = ::mair::mergeKind(Kind, Other);
  refreshOptimizationProperties();
  if (ProgramPoint *PP = getParent())
    PP->invalidateSort();
}

void Observation::eraseFromParent() {
  ProgramPoint *PP = getParent();
  assert(PP && "observation has no parent");
  PP->erase(*this);
}

const Value *Observation::getBase() const {
  return std::visit([](const auto &R) { return R.getBase(); }, Region);
}

bool Observation::hasConcreteRange() const {
  const AbstractAccess *Location = getContiguousRegion();
  return Location && Location->getOffset().isConstant() &&
         Location->getSize().isConstant();
}

bool Observation::covers(const Observation &Other) const {
  // The witness may be pinned. Only the observation being deleted must be
  // deletable, so a retained memset can cover a later ordinary store.
  if (getBase() != Other.getBase() || !Other.isDeletable() ||
      getLevel() < Other.getLevel())
    return false;
  if (isAtomic() != Other.isAtomic() &&
      (!isMergeable() || !Other.isMergeable()))
    return false;

  if (const auto *ThisAccess = getContiguousRegion()) {
    if (const auto *OtherAccess = Other.getContiguousRegion())
      return ThisAccess->contains(*OtherAccess);
    if (const auto *OtherMasked = Other.getMaskedRegion())
      return ThisAccess->contains(OtherMasked->getBounds());
    return false;
  }
  if (const auto *ThisMasked = getMaskedRegion()) {
    const auto *OtherMasked = Other.getMaskedRegion();
    return OtherMasked && ThisMasked->contains(*OtherMasked);
  }
  const auto *ThisPeriodic = getPeriodicRegion();
  const auto *OtherPeriodic = Other.getPeriodicRegion();
  return ThisPeriodic && OtherPeriodic && *ThisPeriodic == *OtherPeriodic;
}

bool Observation::mergeWith(const Observation &Other, ScalarEvolution *SE) {
  if (getBase() != Other.getBase())
    return false;
  if (!isMergeable() || !Other.isMergeable())
    return false;
  if (getLevel() != Other.getLevel())
    return false;

  bool PrependOther = false;
  if (const auto *ThisAccess = getContiguousRegion()) {
    const auto *OtherAccess = Other.getContiguousRegion();
    if (!OtherAccess)
      return false;
    const Instruction *Provenance = getInstruction();
    const DataLayout *DL =
        Provenance ? &Provenance->getModule()->getDataLayout() : nullptr;
    MAIRGraph *ExprGraph = nullptr;
    if (ProgramPoint *PP = getParent())
      ExprGraph = PP->getParent();
    std::optional<AbstractAccess> Merged =
        AbstractAccess::merge(*ThisAccess, *OtherAccess, DL, SE, ExprGraph);
    if (!Merged)
      return false;
    const bool ThisContainsOther = *Merged == *ThisAccess;
    const bool OtherContainsThis = *Merged == *OtherAccess;
    PrependOther = OtherContainsThis && !ThisContainsOther;
    Region = *Merged;
  } else if (const auto *ThisMasked = getMaskedRegion()) {
    const auto *OtherMasked = Other.getMaskedRegion();
    if (!OtherMasked)
      return false;
    const bool ThisContainsOther = ThisMasked->contains(*OtherMasked);
    const bool OtherContainsThis = OtherMasked->contains(*ThisMasked);
    std::optional<MaskedRegion> Merged =
        MaskedRegion::merge(*ThisMasked, *OtherMasked);
    if (!Merged)
      return false;
    PrependOther = OtherContainsThis && !ThisContainsOther;
    Region = std::move(*Merged);
  } else {
    const auto *ThisPeriodic = getPeriodicRegion();
    const auto *OtherPeriodic = Other.getPeriodicRegion();
    if (!ThisPeriodic || !OtherPeriodic || *ThisPeriodic != *OtherPeriodic)
      return false;
  }

  if (PrependOther) {
    SmallVector<Instruction *, 2> OrderedInsts;
    OrderedInsts.append(Other.Insts.begin(), Other.Insts.end());
    OrderedInsts.append(Insts.begin(), Insts.end());
    Insts = std::move(OrderedInsts);
  } else {
    Insts.append(Other.Insts);
  }

  Kind = ::mair::mergeKind(Kind, Other.Kind);
  Properties |= Other.Properties;
  refreshOptimizationProperties();
  if (ProgramPoint *PP = getParent())
    PP->invalidateSort();
  return true;
}

DebugLoc Observation::getStableDebugLoc() const {
  for (const Instruction *I : Insts) {
    const DebugLoc &Loc = I->getStableDebugLoc();
    if (Loc)
      return Loc;
  }
  return DebugLoc();
}

std::string Observation::toString() const { return printToString(*this); }

Observation Observation::fromInstruction(const Instruction &I,
                                         const DataLayout &DL) {
  const Value *Ptr = nullptr;
  AbstractValue Size = AbstractValue::unknown();
  AccessKind Kind = AccessKind::Read;
  if (isa<LoadInst, StoreInst, AtomicRMWInst, AtomicCmpXchgInst>(I)) {
    // LLVM knows the pointer and the store size of these accesses.
    MemoryLocation Location = MemoryLocation::get(&I);
    Ptr = Location.Ptr;
    if (Location.Size.hasValue() && !Location.Size.isScalable())
      Size = AbstractValue::constant(Location.Size.getValue().getFixedValue());
    Kind = isa<LoadInst>(I)    ? AccessKind::Read
           : isa<StoreInst>(I) ? AccessKind::Write
                               : AccessKind::ReadWrite;
  } else if (isa<AnyMemTransferInst>(I)) {
    report_fatal_error(
        "Observation::fromInstruction cannot represent a memory transfer");
  } else if (const auto *MI = dyn_cast<AnyMemSetInst>(&I)) {
    Ptr = MI->getRawDest();
    Size = getAccessSize(*MI->getLength());
    Kind = AccessKind::Write;
  } else if (const auto *II = dyn_cast<IntrinsicInst>(&I);
             II && isMaskedMemoryIntrinsic(I)) {
    bool IsWrite = II->getIntrinsicID() == Intrinsic::masked_store;
    unsigned PtrOperand = IsWrite ? 1 : 0;
    unsigned MaskOperand = IsWrite ? 2 : 1;
    Type *AccessType =
        IsWrite ? II->getArgOperand(0)->getType() : II->getType();
    Ptr = II->getArgOperand(PtrOperand);
    Size = getStoreSize(*AccessType, DL);
    Kind = IsWrite ? AccessKind::Write : AccessKind::Read;
    auto *VectorAccessType = cast<VectorType>(AccessType);
    TypeSize ElementStoreSize =
        DL.getTypeStoreSize(VectorAccessType->getElementType());
    if (ElementStoreSize.isScalable())
      report_fatal_error("masked access has scalable element size");
    AbstractAccess Bounds = AbstractAccess::fromPointerAndSize(*Ptr, Size, DL);
    const Value &Mask = *II->getArgOperand(MaskOperand);
    std::optional<SmallBitVector> ConstantMask = getConstantMask(Mask);
    MaskedRegion Region =
        ConstantMask
            ? MaskedRegion(Bounds, std::move(*ConstantMask),
                           ElementStoreSize.getFixedValue())
            : MaskedRegion(Bounds, Mask, ElementStoreSize.getFixedValue(),
                           /*Comparable=*/!isa<Constant>(Mask));
    Instruction *Inst = const_cast<Instruction *>(&I);
    return Observation(ArrayRef<Instruction *>(&Inst, 1),
                       MemoryRegion(std::move(Region)), Kind,
                       getInstructionProperties(I));
  } else if (I.mayReadFromMemory() && I.mayWriteToMemory()) {
    Kind = AccessKind::ReadWrite;
  } else if (I.mayWriteToMemory()) {
    Kind = AccessKind::Write;
  } else if (!I.mayReadFromMemory()) {
    report_fatal_error(
        "Observation::fromInstruction expects a memory access instruction");
  }

  if (!Ptr)
    report_fatal_error("memory access instruction has no modeled pointer");
  return makeObservation(I, *Ptr, std::move(Size), Kind, DL);
}

void Observation::print(raw_ostream &OS) const {
  OS << "o: " << getAccessKindName(Kind);
  if (isVolatile())
    OS << " volatile";
  if (isAtomic())
    OS << " atomic";
  if (isCallLike())
    OS << " call";
  if (isMasked())
    OS << " masked";
  if (const auto *Obs = getContiguousRegion()) {
    OS << " ptr ";
    Obs->print(OS);
    OS << ", ";
    if (Obs->hasSizeExpr())
      expressionSize(*Obs).print(OS);
    else
      printAbstractValue(OS, Obs->getSize());
    return;
  }
  if (const MaskedRegion *Masked = getMaskedRegion()) {
    OS << " ";
    Masked->print(OS);
    return;
  }
  OS << " ";
  getPeriodicRegion()->print(OS);
}

} // namespace mair
