#include "MAIR/MAIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using namespace llvm;

namespace mair {

struct MAIRScalarExprOwner {};

struct MAIRScalarExprNode {
  const MAIRScalarExprOwner *Owner = nullptr;
  MAIRScalarExpr::Kind Kind = MAIRScalarExpr::Kind::Constant;
  Type *ResultType = nullptr;
  APInt Constant = APInt(1, 0);
  const Value *LLVMValue = nullptr;
  SmallVector<MAIRScalarExpr, 2> Operands;

  MAIRScalarExprNode(const MAIRScalarExprOwner &O, MAIRScalarExpr::Kind K,
                     Type &Ty, const APInt &C, const Value *V,
                     ArrayRef<MAIRScalarExpr> Ops)
      : Owner(&O), Kind(K), ResultType(&Ty), Constant(C), LLVMValue(V),
        Operands(Ops) {}
};

namespace {
const Instruction *getBarrierRangeStart(const ProgramPoint &PP) {
  return PP.isBasicBlockEntrySite() ? nullptr : &PP.getAnchor();
}

const Instruction *getBarrierRangeEnd(const ProgramPoint &PP) {
  return &PP.getAnchor();
}

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

bool isInterestingMop(const Instruction &I) {
  if (I.hasMetadata(LLVMContext::MD_nosanitize))
    return false;
  return isa<LoadInst, StoreInst, AtomicRMWInst, AtomicCmpXchgInst,
             AnyMemIntrinsic>(I) ||
         isMaskedMemoryIntrinsic(I);
}

} // namespace

// Build the initial, unoptimized MAIR graph for one LLVM function.
//
// ProgramPoint layout follows each LLVM basic block:
//   BBEntry -> memory-instruction program points in source order -> BBExit.
// Edges are then added for adjacent program points inside each block and for
// immediate dominator/post-dominator relationships across blocks. Barrier
// queries decide whether the corresponding forward/backward sanitizer
// reachability edge is legal.
MAIRGraph MAIRGraph::build(Function &F, const DominatorTree &DT,
                           const PostDominatorTree &PDT, const LoopInfo &LI,
                           MAIRBarrierAnalysisCore &BA,
                           MAIROptimizationConfig Config) {
  MAIRGraph Graph(F, Config);
  Graph.Analyses.bind(DT, PDT, LI);
  DenseMap<const BasicBlock *, ProgramPoint *> BBEntryPPs;
  DenseMap<const BasicBlock *, ProgramPoint *> BBExitPPs;
  DenseMap<const BasicBlock *, SmallVector<ProgramPoint *, 4>> OrderedPPs;

  // ===========================================================
  // 1. Create program points for memory instructions and block boundaries
  // ===========================================================
  for (BasicBlock &BB : F) {
    auto &PPsInOneBB = OrderedPPs[&BB];
    auto &Start = Graph.createBBProgramPoint(BB, true);
    BBEntryPPs[&BB] = &Start;
    PPsInOneBB.push_back(&Start);
    for (Instruction &I : BB) {
      if (isInterestingMop(I)) {
        auto &PP = Graph.createInstProgramPoint(BB, I);
        PPsInOneBB.push_back(&PP);
      }
    }
    auto &End = Graph.createBBProgramPoint(BB, false);
    BBExitPPs[&BB] = &End;
    PPsInOneBB.push_back(&End);
  }

  // ===========================================================
  // 2. Connect program point edges inside each block
  // ===========================================================
  for (auto &Entry : OrderedPPs) {
    const BasicBlock &BB = *Entry.first;
    ArrayRef<ProgramPoint *> Points = Entry.second;
    assert(Points.size() > 1);
    for (unsigned I = 1; I < Points.size(); ++I) {
      ProgramPoint &Prev = *Points[I - 1], &Next = *Points[I];
      const Instruction *PrevI = getBarrierRangeStart(Prev);
      const Instruction *NextI = getBarrierRangeEnd(Next);
      bool FwdEndpointBarrier =
          Prev.isMemoryInstSite() && PrevI && BA.isFwdBarrier(*PrevI);
      bool BwdEndpointBarrier =
          Prev.isMemoryInstSite() && PrevI && BA.isBwdBarrier(*PrevI);
      if (!FwdEndpointBarrier && !BA.hasFwdBarrierBetween(BB, PrevI, NextI))
        Graph.addEdge(Prev, Next, PPEdgeKind::Dom);
      if (!BwdEndpointBarrier && !BA.hasBwdBarrierBetween(BB, PrevI, NextI))
        Graph.addEdge(Next, Prev, PPEdgeKind::PostDom);
    }
  }

  // ===========================================================
  // 3. Connect program point edges across blocks via DT/PDT
  // ===========================================================
  auto GetIDom = [](auto &DT, const BasicBlock &BB) -> const BasicBlock * {
    if (const DomTreeNode *Node = DT.getNode(&BB))
      if (const DomTreeNode *IDom = Node->getIDom())
        return IDom->getBlock();
    return nullptr;
  };
  for (BasicBlock &BB : F) {
    ProgramPoint *EntryPP = BBEntryPPs.lookup(&BB);
    ProgramPoint *ExitPP = BBExitPPs.lookup(&BB);
    assert(EntryPP && ExitPP);
    if (const BasicBlock *IDom = GetIDom(DT, BB)) {
      ProgramPoint *Pred = BBExitPPs.lookup(IDom);
      assert(Pred);
      if (!BA.isFwdBarrier(*IDom->getTerminator()) &&
          !BA.hasFwdBarrierPath(*IDom, BB))
        Graph.addEdge(*Pred, *EntryPP, PPEdgeKind::Dom);
    }
    if (const BasicBlock *IPDom = GetIDom(PDT, BB)) {
      ProgramPoint *Succ = BBEntryPPs.lookup(IPDom);
      assert(Succ);
      if (!BA.isBwdBarrier(*BB.getTerminator()) &&
          !BA.hasBwdBarrierPath(BB, *IPDom))
        Graph.addEdge(*Succ, *ExitPP, PPEdgeKind::PostDom);
    }
  }

  return Graph;
}

namespace {
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

static std::optional<uint64_t> getNonNegativeU64(const APInt &Value) {
  if (Value.isNegative() || Value.getActiveBits() > 64)
    return std::nullopt;
  return Value.getZExtValue();
}

static std::optional<uint64_t> checkedEnd(uint64_t Begin, uint64_t Size) {
  if (UINT64_MAX - Begin < Size)
    return std::nullopt;
  return Begin + Size;
}

static AbstractValue getStoreSize(Type &Ty, const DataLayout &DL) {
  TypeSize Size = DL.getTypeStoreSize(&Ty);
  if (Size.isScalable())
    return AbstractValue::unknown();
  return AbstractValue::constant(Size.getFixedValue());
}

static AbstractValue getAccessSize(const Value &Value) {
  const auto *Size = dyn_cast<ConstantInt>(&Value);
  if (!Size || Size->getValue().isNegative() ||
      Size->getValue().getActiveBits() > 64)
    return AbstractValue::symbol(Value);
  return AbstractValue::constant(Size->getZExtValue());
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

static Observation makeObservation(const Instruction &I, const Value &Ptr,
                                   AbstractValue Size, AccessKind Kind,
                                   const DataLayout &DL) {
  AbstractAccess Obs =
      AbstractAccess::fromPointerAndSize(Ptr, std::move(Size), DL);
  Instruction *Inst = const_cast<Instruction *>(&I);
  return Observation(ArrayRef<Instruction *>(&Inst, 1), std::move(Obs), Kind,
                     getInstructionProperties(I));
}

static void printAbstractValue(raw_ostream &OS, const AbstractValue &Value) {
  if (Value.isUnknown()) {
    OS << "unknown";
    return;
  }
  if (Value.isConstant()) {
    OS << Value.getConstant();
    return;
  }
  OS << getPrintedValueName(*Value.getSymbol());
}

static void printAccessKind(raw_ostream &OS, AccessKind Kind) {
  switch (Kind) {
  case AccessKind::Read:
    OS << "read";
    break;
  case AccessKind::Write:
    OS << "write";
    break;
  case AccessKind::ReadWrite:
    OS << "read/write";
    break;
  }
}

static void printEdgeKind(raw_ostream &OS, PPEdgeKind Kind) {
  switch (Kind) {
  case PPEdgeKind::Dom:
    OS << "dom";
    break;
  case PPEdgeKind::PostDom:
    OS << "pdom";
    break;
  }
}

static void printEdgeList(raw_ostream &OS, ArrayRef<ProgramPointEdge> Edges) {
  bool First = true;
  for (const ProgramPointEdge &Edge : Edges) {
    if (!First)
      OS << ", ";
    OS << Edge.getNode().getName() << "(";
    printEdgeKind(OS, Edge.getKind());
    OS << ")";
    First = false;
  }
}

} // namespace

ProgramPointSite ProgramPointSite::anchored(const Instruction &Anchor, Kind K) {
  assert(K != Kind::Edge && "an anchored site cannot have edge kind");
  assert(Anchor.getParent() && "a program point anchor must belong to a block");
  ProgramPointSite Site;
  Site.SiteKind = K;
  Site.Anchor = &Anchor;
  return Site;
}

ProgramPointSite ProgramPointSite::edge(const BasicBlock &From,
                                        const BasicBlock &To) {
  const Instruction *Anchor = From.getTerminator();
  assert(Anchor && "an edge program point source must have a terminator");
  ProgramPointSite Site;
  Site.SiteKind = Kind::Edge;
  Site.Anchor = Anchor;
  Site.From = &From;
  Site.To = &To;
  return Site;
}

void ProgramPointSite::print(raw_ostream &OS) const {
  if (!isEdge()) {
    switch (SiteKind) {
    case Kind::MemoryInst:
      OS << "memory(";
      break;
    case Kind::BasicBlockEntry:
      OS << "bb.entry(";
      break;
    case Kind::BasicBlockExit:
      OS << "bb.exit(";
      break;
    case Kind::Edge:
      llvm_unreachable("handled edge program point site");
    }
    getBasicBlock()->printAsOperand(OS, false);
    if (SiteKind == Kind::MemoryInst) {
      OS << ", ";
      getAnchor().printAsOperand(OS, false);
    }
    OS << ")";
    return;
  }
  OS << "edge(";
  if (From)
    From->printAsOperand(OS, false);
  else
    OS << "null";
  OS << " -> ";
  if (To)
    To->printAsOperand(OS, false);
  else
    OS << "null";
  OS << ")";
}

bool AnyProvenanceExecutedGuard::operator==(
    const AnyProvenanceExecutedGuard &Other) const {
  if (ResetBlock != Other.ResetBlock ||
      Provenance.size() != Other.Provenance.size())
    return false;
  return llvm::all_of(Provenance,
                      [&](const Instruction *I) {
                        return llvm::is_contained(Other.Provenance, I);
                      }) &&
         llvm::all_of(Other.Provenance, [&](const Instruction *I) {
           return llvm::is_contained(Provenance, I);
         });
}

MAIRScalarExpr::Kind MAIRScalarExpr::getKind() const {
  assert(Node && "invalid MAIR scalar expression");
  return Node->Kind;
}

Type *MAIRScalarExpr::getType() const {
  assert(Node && "invalid MAIR scalar expression");
  return Node->ResultType;
}

const APInt &MAIRScalarExpr::getConstant() const {
  assert(Node && Node->Kind == Kind::Constant &&
         "MAIR scalar expression is not a constant");
  return Node->Constant;
}

const Value *MAIRScalarExpr::getLLVMValue() const {
  assert(Node && Node->Kind == Kind::LLVMValue &&
         "MAIR scalar expression is not an LLVM value");
  return Node->LLVMValue;
}

ArrayRef<MAIRScalarExpr> MAIRScalarExpr::operands() const {
  assert(Node && "invalid MAIR scalar expression");
  return Node->Operands;
}

const MAIRScalarExprOwner *MAIRScalarExpr::getOwner() const {
  return Node ? Node->Owner : nullptr;
}

bool MAIRScalarExpr::operator==(const MAIRScalarExpr &Other) const {
  if (Node == Other.Node)
    return true;
  if (!Node || !Other.Node || Node->Kind != Other.Node->Kind ||
      Node->ResultType != Other.Node->ResultType ||
      Node->LLVMValue != Other.Node->LLVMValue ||
      Node->Operands.size() != Other.Node->Operands.size())
    return false;
  if (Node->Kind == Kind::Constant && Node->Constant != Other.Node->Constant)
    return false;
  for (size_t I = 0; I < Node->Operands.size(); ++I)
    if (Node->Operands[I] != Other.Node->Operands[I])
      return false;
  return true;
}

void MAIRScalarExpr::print(raw_ostream &OS, bool Signed) const {
  if (!Node) {
    OS << "<invalid>";
    return;
  }
  if (Node->Kind == Kind::Constant) {
    SmallString<32> Text;
    Node->Constant.toString(Text, 10, Signed);
    OS << Text;
    return;
  }
  if (Node->Kind == Kind::LLVMValue) {
    if (Node->LLVMValue)
      OS << getPrintedValueName(*Node->LLVMValue);
    else
      OS << "<null>";
    return;
  }

  auto PrintBinary = [&](StringRef Operator) {
    OS << "(";
    if (!Node->Operands.empty())
      Node->Operands[0].print(OS, Signed);
    OS << " " << Operator << " ";
    if (Node->Operands.size() > 1)
      Node->Operands[1].print(OS, Signed);
    OS << ")";
  };
  switch (Node->Kind) {
  case Kind::Add:
    return PrintBinary("+");
  case Kind::Mul:
    return PrintBinary("*");
  case Kind::UDiv:
    return PrintBinary("udiv");
  case Kind::SMin:
    return PrintBinary("smin");
  case Kind::UMin:
    return PrintBinary("umin");
  case Kind::SMax:
    return PrintBinary("smax");
  case Kind::UMax:
    return PrintBinary("umax");
  case Kind::Truncate:
    OS << "trunc(";
    break;
  case Kind::ZeroExtend:
    OS << "zext(";
    break;
  case Kind::SignExtend:
    OS << "sext(";
    break;
  case Kind::Constant:
  case Kind::LLVMValue:
    llvm_unreachable("handled scalar expression leaf");
  }
  if (!Node->Operands.empty())
    Node->Operands.front().print(OS, Signed);
  OS << " to ";
  if (Node->ResultType)
    Node->ResultType->print(OS);
  else
    OS << "<null>";
  OS << ")";
}

void PeriodicRegion::print(raw_ostream &OS) const {
  OS << "period start=";
  Start.print(OS);
  OS << ", stride=";
  Stride.print(OS, true);
  OS << ", access-size=";
  AccessSize.print(OS);
  OS << ", trips=";
  TripCount.print(OS);
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

AbstractValue::Kind AbstractValue::getKind() const {
  if (isUnknown())
    return Kind::Unknown;
  if (isSymbol())
    return Kind::Symbol;
  return Kind::Constant;
}

AbstractValue AbstractValue::symbol(const Value &V) {
  AbstractValue Result;
  Result.Storage = &V;
  return Result;
}

AbstractValue AbstractValue::constant(uint64_t C) {
  AbstractValue Result;
  Result.Storage = C;
  return Result;
}

AbstractAccess AbstractAccess::fromPointerAndSize(const Value &Ptr,
                                                  AbstractValue Size,
                                                  const DataLayout &DL) {
  APInt Offset(DL.getIndexTypeSizeInBits(Ptr.getType()), 0);
  const Value *Stripped = Ptr.stripAndAccumulateConstantOffsets(
      DL, Offset, /*AllowNonInbounds=*/true);
  const Value *Underlying = getUnderlyingObject(&Ptr);
  if (!Underlying)
    Underlying = &Ptr;

  std::optional<uint64_t> ConcreteOffset = getNonNegativeU64(Offset);
  if (Stripped == Underlying && ConcreteOffset)
    return AbstractAccess(Underlying, AbstractValue::constant(*ConcreteOffset),
                          Size);

  if (Underlying != &Ptr)
    return AbstractAccess(Underlying, AbstractValue::symbol(Ptr), Size);

  return AbstractAccess(&Ptr, AbstractValue::constant(0), Size);
}

bool AbstractAccess::contains(const AbstractAccess &Other) const {
  if (*this == Other)
    return true;
  if (Base != Other.Base)
    return false;
  if (!Offset.isConstant() || !Size.isConstant() ||
      !Other.Offset.isConstant() || !Other.Size.isConstant())
    return false;

  uint64_t ThisBegin = Offset.getConstant();
  std::optional<uint64_t> ThisEnd = checkedEnd(ThisBegin, Size.getConstant());
  uint64_t OtherBegin = Other.Offset.getConstant();
  std::optional<uint64_t> OtherEnd =
      checkedEnd(OtherBegin, Other.Size.getConstant());
  if (!ThisEnd || !OtherEnd)
    return false;
  return ThisBegin <= OtherBegin && *OtherEnd <= *ThisEnd;
}

std::optional<AbstractAccess> AbstractAccess::merge(const AbstractAccess &A,
                                                    const AbstractAccess &B,
                                                    const DataLayout *DL) {
  if (A == B)
    return A;
  if (A.Base != B.Base)
    return std::nullopt;
  if (A.contains(B))
    return A;
  if (B.contains(A))
    return B;
  if (!A.Size.isConstant() || !B.Size.isConstant())
    return std::nullopt;

  if (!A.Offset.isConstant() || !B.Offset.isConstant()) {
    if (!DL || !A.Offset.isSymbol() || !B.Offset.isSymbol())
      return std::nullopt;
    const Value *AStart = A.Offset.getSymbol();
    const Value *BStart = B.Offset.getSymbol();
    if (!AStart->getType()->isPointerTy() || !BStart->getType()->isPointerTy())
      return std::nullopt;

    std::optional<int64_t> PointerDelta =
        BStart->getPointerOffsetFrom(AStart, *DL);
    if (!PointerDelta)
      return std::nullopt;

    APInt BBegin(128, static_cast<uint64_t>(*PointerDelta), true);
    APInt AEnd(128, A.Size.getConstant());
    APInt BEnd = BBegin + APInt(128, B.Size.getConstant());
    APInt Zero(128, 0);
    if (AEnd.slt(BBegin) || BEnd.slt(Zero))
      return std::nullopt;

    const APInt &Begin = BBegin.slt(Zero) ? BBegin : Zero;
    const APInt &End = AEnd.sgt(BEnd) ? AEnd : BEnd;
    APInt MergedSize = End - Begin;
    if (MergedSize.getActiveBits() > 64)
      return std::nullopt;

    const AbstractAccess &First = BBegin.slt(Zero) ? B : A;
    return AbstractAccess(First.Base, First.Offset,
                          AbstractValue::constant(MergedSize.getZExtValue()));
  }

  uint64_t ABegin = A.Offset.getConstant();
  std::optional<uint64_t> AEnd = checkedEnd(ABegin, A.Size.getConstant());
  uint64_t BBegin = B.Offset.getConstant();
  std::optional<uint64_t> BEnd = checkedEnd(BBegin, B.Size.getConstant());
  if (!AEnd || !BEnd)
    return std::nullopt;
  if (*AEnd < BBegin || *BEnd < ABegin)
    return std::nullopt;

  uint64_t Begin = std::min(ABegin, BBegin);
  uint64_t End = std::max(*AEnd, *BEnd);

  return AbstractAccess(A.Base, AbstractValue::constant(Begin),
                        AbstractValue::constant(End - Begin));
}

void AbstractAccess::print(raw_ostream &OS) const {
  if (!Base && Offset.isUnknown()) {
    OS << "<unknown>";
    return;
  }
  if (Base)
    OS << getPrintedValueName(*Base);
  else
    OS << "unknown";
  if (Offset.isConstant()) {
    if (Offset.getConstant() != 0)
      OS << " + " << Offset.getConstant();
  } else {
    OS << " + ";
    printAbstractValue(OS, Offset);
  }
}

MaskedRegion::MaskedRegion(AbstractAccess B, const Value &Mask,
                           uint64_t ElementBytes, bool Comparable)
    : Bounds(std::move(B)), DynamicMask(&Mask), ElementSize(ElementBytes),
      IsComparable(Comparable) {}

MaskedRegion::MaskedRegion(AbstractAccess B, SmallBitVector Mask,
                           uint64_t ElementBytes)
    : Bounds(std::move(B)), ConstantMask(std::move(Mask)),
      ElementSize(ElementBytes), IsConstant(true) {}

unsigned MaskedRegion::getNumLanes() const {
  if (IsConstant)
    return ConstantMask.size();
  const auto *MaskType = dyn_cast<VectorType>(DynamicMask->getType());
  assert(MaskType && "masked region mask must be a vector");
  return MaskType->getElementCount().getKnownMinValue();
}

bool MaskedRegion::contains(const MaskedRegion &Other) const {
  if (!IsComparable || !Other.IsComparable)
    return false;
  if (Bounds != Other.Bounds || ElementSize != Other.ElementSize ||
      getNumLanes() != Other.getNumLanes())
    return false;
  if (!IsConstant || !Other.IsConstant)
    return !IsConstant && !Other.IsConstant && DynamicMask == Other.DynamicMask;
  for (int Lane = Other.ConstantMask.find_first(); Lane >= 0;
       Lane = Other.ConstantMask.find_next(Lane))
    if (!ConstantMask.test(Lane))
      return false;
  return true;
}

std::optional<MaskedRegion> MaskedRegion::merge(const MaskedRegion &A,
                                                const MaskedRegion &B) {
  if (!A.IsComparable || !B.IsComparable)
    return std::nullopt;
  if (A.Bounds != B.Bounds || A.ElementSize != B.ElementSize ||
      A.getNumLanes() != B.getNumLanes())
    return std::nullopt;
  if (!A.IsConstant || !B.IsConstant) {
    if (!A.IsConstant && !B.IsConstant && A.DynamicMask == B.DynamicMask)
      return A;
    return std::nullopt;
  }
  SmallBitVector Combined = A.ConstantMask;
  Combined |= B.ConstantMask;
  return MaskedRegion(A.Bounds, std::move(Combined), A.ElementSize);
}

bool MaskedRegion::operator==(const MaskedRegion &Other) const {
  if (Bounds != Other.Bounds || ElementSize != Other.ElementSize ||
      IsConstant != Other.IsConstant || IsComparable != Other.IsComparable)
    return false;
  return IsConstant ? ConstantMask == Other.ConstantMask
                    : DynamicMask == Other.DynamicMask;
}

void MaskedRegion::print(raw_ostream &OS) const {
  OS << "ptr ";
  Bounds.print(OS);
  OS << ", ";
  printAbstractValue(OS, Bounds.getSize());
  OS << ", mask ";
  if (!IsConstant) {
    OS << getPrintedValueName(*DynamicMask);
    return;
  }
  OS << "<";
  for (unsigned Lane = 0; Lane < ConstantMask.size(); ++Lane) {
    if (Lane)
      OS << ",";
    OS << (ConstantMask.test(Lane) ? '1' : '0');
  }
  OS << ">";
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
                                /*AllowRelocation=*/false);
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

void MAIROptimizationConfig::initializeObservation(Observation &Obs) const {
  const MaskedRegion *Masked = Obs.getMaskedRegion();
  const bool IsComparableMasked = !Masked || Masked->isComparable();
  const bool CanMerge = !Obs.isVolatile() && !Obs.isCallLike() &&
                        IsComparableMasked && (IgnoreAtomic || !Obs.isAtomic());
  const bool CanMove = AllowRelocation && !Obs.isVolatile() &&
                       !Obs.isCallLike() && !Obs.isAtomic();
  const bool CanDelete = !Obs.isVolatile() && !Obs.isCallLike() &&
                         (IgnoreAtomic || !Obs.isAtomic());
  Obs.setOptimizationProperties(CanMerge, CanMove, CanDelete,
                                getLevel(Obs.getKind()));
}

void Observation::refreshOptimizationProperties() {
  if (ProgramPoint *PP = getParent())
    if (MAIRGraph *Graph = PP->getParent())
      return Graph->getOptimizationConfig().initializeObservation(*this);
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
  if (getBase() != Other.getBase() || !isDeletable() || !Other.isDeletable() ||
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

bool Observation::mergeWith(const Observation &Other) {
  if (getBase() != Other.getBase())
    return false;
  if (!isMergeable() || !Other.isMergeable())
    return false;
  if (getLevel() != Other.getLevel())
    return false;

  if (const auto *ThisAccess = getContiguousRegion()) {
    const auto *OtherAccess = Other.getContiguousRegion();
    if (!OtherAccess)
      return false;
    const Instruction *Provenance = getInstruction();
    const DataLayout *DL =
        Provenance ? &Provenance->getModule()->getDataLayout() : nullptr;
    std::optional<AbstractAccess> Merged =
        AbstractAccess::merge(*ThisAccess, *OtherAccess, DL);
    if (!Merged)
      return false;
    const bool ThisContainsOther = *Merged == *ThisAccess;
    const bool OtherContainsThis = *Merged == *OtherAccess;
    Region = *Merged;
    if (OtherContainsThis && !ThisContainsOther) {
      SmallVector<Instruction *, 2> OrderedInsts;
      OrderedInsts.append(Other.Insts.begin(), Other.Insts.end());
      OrderedInsts.append(Insts.begin(), Insts.end());
      Insts = std::move(OrderedInsts);
    } else {
      Insts.append(Other.Insts);
    }
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
    Region = std::move(*Merged);
    if (OtherContainsThis && !ThisContainsOther) {
      SmallVector<Instruction *, 2> OrderedInsts;
      OrderedInsts.append(Other.Insts.begin(), Other.Insts.end());
      OrderedInsts.append(Insts.begin(), Insts.end());
      Insts = std::move(OrderedInsts);
    } else {
      Insts.append(Other.Insts);
    }
  } else {
    const auto *ThisPeriodic = getPeriodicRegion();
    const auto *OtherPeriodic = Other.getPeriodicRegion();
    if (!ThisPeriodic || !OtherPeriodic || *ThisPeriodic != *OtherPeriodic)
      return false;
    Insts.append(Other.Insts);
  }

  Kind = ::mair::mergeKind(Kind, Other.Kind);
  Properties |= Other.Properties;
  refreshOptimizationProperties();
  if (ProgramPoint *PP = getParent())
    PP->invalidateSort();
  return true;
}

std::string Observation::toString() const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS);
  OS.flush();
  return S;
}

Observation Observation::fromInstruction(const Instruction &I,
                                         const DataLayout &DL) {
  const Value *Ptr = nullptr;
  AbstractValue Size = AbstractValue::unknown();
  AccessKind Kind = AccessKind::Read;
  if (const auto *LI = dyn_cast<LoadInst>(&I)) {
    Ptr = LI->getPointerOperand();
    Size = getStoreSize(*LI->getType(), DL);
    Kind = AccessKind::Read;
  } else if (const auto *SI = dyn_cast<StoreInst>(&I)) {
    Ptr = SI->getPointerOperand();
    Size = getStoreSize(*SI->getValueOperand()->getType(), DL);
    Kind = AccessKind::Write;
  } else if (const auto *RMW = dyn_cast<AtomicRMWInst>(&I)) {
    Ptr = RMW->getPointerOperand();
    Size = getStoreSize(*RMW->getValOperand()->getType(), DL);
    Kind = AccessKind::ReadWrite;
  } else if (const auto *XCHG = dyn_cast<AtomicCmpXchgInst>(&I)) {
    Ptr = XCHG->getPointerOperand();
    Size = getStoreSize(*XCHG->getCompareOperand()->getType(), DL);
    Kind = AccessKind::ReadWrite;
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
  OS << "o: ";
  printAccessKind(OS, Kind);
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

ProgramPoint::ProgramPoint(StringRef N, const Instruction &Anchor, SiteKind K)
    : ProgramPoint(N, ProgramPointSite::anchored(Anchor, K), AlwaysGuard{}) {}

ProgramPoint::ProgramPoint(StringRef N, ProgramPointSite S, ProgramPointGuard G)
    : Name(N.str()), Site(std::move(S)), Guard(std::move(G)) {
  assert(Site.getBasicBlock() && "a program point must have an anchor");
  if (!Site.isEdge())
    RepresentedBlocks.insert(Site.getBasicBlock());
}

ProgramPoint::~ProgramPoint() { ObservationAllocator.DestroyAll(); }

Observation &ProgramPoint::createObservation(ArrayRef<Instruction *> Insts,
                                             const MemoryRegion &Region,
                                             AccessKind Kind,
                                             AccessProperty Properties) {
  return *new (ObservationAllocator.Allocate())
      Observation(Insts, Region, Kind, Properties);
}

Observation &ProgramPoint::addObservation(const Observation &A) {
  auto &Node = createObservation(A.getInstructions(), A.getRegion(),
                                 A.getKind(), A.getProperties());
  Observations.push_back(Node);
  Node.setParent(this);
  if (MAIRGraph *Graph = getParent())
    Graph->registerObservation(Node);
  ++NumObservations;
  Sorted = false;
  return Node;
}

bool ProgramPoint::hasChild(const ProgramPoint &PP, PPEdgeKind Kind) const {
  return llvm::any_of(children(), [&](const ProgramPointEdge &Edge) {
    return &Edge.getNode() == &PP && Edge.getKind() == Kind;
  });
}

bool ProgramPoint::hasParent(const ProgramPoint &PP, PPEdgeKind Kind) const {
  return llvm::any_of(parents(), [&](const ProgramPointEdge &Edge) {
    return &Edge.getNode() == &PP && Edge.getKind() == Kind;
  });
}

bool ProgramPoint::hasCompatibleMergeSemantics(
    const ProgramPoint &Other) const {
  if (Guard != Other.Guard)
    return false;
  if (!isEdgeSite() && !Other.isEdgeSite())
    return getSiteKind() == Other.getSiteKind();
  return isEdgeSite() && Other.isEdgeSite() && Site == Other.Site;
}

void ProgramPoint::sort(ObservationComparator Compare) {
  if (Sorted && SortedWith == Compare)
    return;
  Observations.sort(Compare);
  Sorted = true;
  SortedWith = Compare;
}

ProgramPoint::iterator ProgramPoint::erase(iterator It) {
  assert(It != Observations.end() && "Cannot erase end iterator");
  if (MAIRGraph *Graph = getParent())
    Graph->unregisterObservation(*It);
  else
    It->clearMemoryObject();
  auto Next = Observations.erase(It);
  --NumObservations;
  return Next;
}

ProgramPoint::iterator ProgramPoint::erase(Observation &Obs) {
  assert(Obs.getParent() == this &&
         "observation does not belong to this program point");
  return erase(Obs.getIterator());
}

void ProgramPoint::mergeObservationsFrom(const ProgramPoint &Other) {
  assert(
      hasCompatibleMergeSemantics(Other) &&
      "cannot merge program points with incompatible site or guard semantics");
  for (const Observation &A : Other.Observations)
    addObservation(A);
}

void ProgramPoint::prependObservationsFrom(const ProgramPoint &Other) {
  assert(
      hasCompatibleMergeSemantics(Other) &&
      "cannot merge program points with incompatible site or guard semantics");
  for (const Observation &A : llvm::reverse(Other.Observations)) {
    auto &Node = createObservation(A.getInstructions(), A.getRegion(),
                                   A.getKind(), A.getProperties());
    Observations.push_front(Node);
    Node.setParent(this);
    if (MAIRGraph *Graph = getParent())
      Graph->registerObservation(Node);
    ++NumObservations;
    Sorted = false;
  }
}

void ProgramPoint::mergeRepresentationFrom(const ProgramPoint &Other) {
  RepresentedBlocks.insert_range(Other.RepresentedBlocks);
}

std::string ProgramPoint::toString() const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS);
  OS.flush();
  return S;
}

void ProgramPoint::print(raw_ostream &OS) const {
  OS << Name << ": ";
  if (isEdgeSite() || !hasAlwaysGuard()) {
    OS << "site=";
    Site.print(OS);
    if (const auto *ExecutionGuard =
            std::get_if<AnyProvenanceExecutedGuard>(&Guard)) {
      OS << ", guard=any(";
      bool First = true;
      for (const Instruction *I : ExecutionGuard->getProvenance()) {
        if (!First)
          OS << ", ";
        if (I)
          OS << getPrintedValueName(*I);
        else
          OS << "null";
        First = false;
      }
      OS << ")";
      if (const BasicBlock *Reset = ExecutionGuard->getResetBlock()) {
        OS << ", reset=";
        Reset->printAsOperand(OS, false);
      }
    }
    OS << ", ";
  }
  OS << "preds=[";
  printEdgeList(OS, Parents);
  OS << "], succs=[";
  printEdgeList(OS, Children);
  OS << "]";
  for (const Observation &A : Observations) {
    OS << "\n  ";
    A.print(OS);
  }
  OS << "\n";
}

MAIRGraph::MAIRGraph(MAIROptimizationConfig Config)
    : OptimizationConfig(Config),
      ScalarExprOwner(std::make_unique<MAIRScalarExprOwner>()) {}

void MAIRGraph::assertLive(StringRef Operation) const {
  if (!isLive())
    report_fatal_error(Twine("cannot ") + Operation + " a non-live MAIRGraph");
}

MAIRGraph::MAIRGraph(const Function &Func, MAIROptimizationConfig Config)
    : F(&Func), OptimizationConfig(Config),
      ScalarExprOwner(std::make_unique<MAIRScalarExprOwner>()) {}

MAIRGraph::MAIRGraph(Function &Func, MAIROptimizationConfig Config)
    : F(&Func), MutableF(&Func), OptimizationConfig(Config),
      ScalarExprOwner(std::make_unique<MAIRScalarExprOwner>()) {}

MAIRGraph::MAIRGraph(MAIRGraph &&Other) noexcept
    : F(Other.F), MutableF(Other.MutableF), GraphState(Other.GraphState),
      OptimizationConfig(Other.OptimizationConfig),
      GraphAllocator(std::move(Other.GraphAllocator)),
      MemoryObjectAllocator(std::move(Other.MemoryObjectAllocator)),
      ScalarExprOwner(std::move(Other.ScalarExprOwner)),
      ScalarExprAllocator(std::move(Other.ScalarExprAllocator)),
      Analyses(std::move(Other.Analyses)),
      MemoryObjects(std::move(Other.MemoryObjects)),
      BBEntryRepresentatives(std::move(Other.BBEntryRepresentatives)),
      BBExitRepresentatives(std::move(Other.BBExitRepresentatives)),
      ProgramPoints(std::move(Other.ProgramPoints)),
      NumProgramPoints(Other.NumProgramPoints), NumEdges(Other.NumEdges),
      NextProgramPointID(Other.NextProgramPointID) {
  for (ProgramPoint &PP : ProgramPoints)
    PP.setParent(this);
  Other.F = nullptr;
  Other.MutableF = nullptr;
  Other.Analyses.clearReferences();
  Other.GraphState = State::MovedFrom;
}

MAIRGraph &MAIRGraph::operator=(MAIRGraph &&Other) noexcept {
  if (this == &Other)
    return *this;
  clearStorage();
  F = Other.F;
  MutableF = Other.MutableF;
  GraphState = Other.GraphState;
  OptimizationConfig = Other.OptimizationConfig;
  GraphAllocator = std::move(Other.GraphAllocator);
  MemoryObjectAllocator = std::move(Other.MemoryObjectAllocator);
  ScalarExprOwner = std::move(Other.ScalarExprOwner);
  ScalarExprAllocator = std::move(Other.ScalarExprAllocator);
  Analyses = std::move(Other.Analyses);
  MemoryObjects = std::move(Other.MemoryObjects);
  BBEntryRepresentatives = std::move(Other.BBEntryRepresentatives);
  BBExitRepresentatives = std::move(Other.BBExitRepresentatives);
  ProgramPoints = std::move(Other.ProgramPoints);
  NumProgramPoints = Other.NumProgramPoints;
  NumEdges = Other.NumEdges;
  NextProgramPointID = Other.NextProgramPointID;
  for (ProgramPoint &PP : ProgramPoints)
    PP.setParent(this);
  Other.F = nullptr;
  Other.MutableF = nullptr;
  Other.Analyses.clearReferences();
  Other.GraphState = State::MovedFrom;
  return *this;
}

MAIRGraph::~MAIRGraph() { clearStorage(); }

template <class T, class... Args>
T &MAIRGraph::allocateNode(Args &&...ArgsPack) {
  return *new (GraphAllocator.Allocate()) T(std::forward<Args>(ArgsPack)...);
}

std::string MAIRGraph::createProgramPointName() {
  return (Twine("pp") + Twine(NextProgramPointID++)).str();
}

ProgramPoint &MAIRGraph::addProgramPoint(ProgramPoint &PP) {
  ProgramPoints.push_back(PP);
  PP.setParent(this);
  ++NumProgramPoints;
  return PP;
}

MemoryObject &MAIRGraph::getOrCreateMemoryObject(const Value &Base) {
  MemoryObject *&Object = MemoryObjects[&Base];
  if (!Object)
    Object = new (MemoryObjectAllocator.Allocate()) MemoryObject(Base);
  return *Object;
}

void MAIRGraph::registerObservation(Observation &Obs) {
  const Value *Base = Obs.getBase();
  assert(Base && "cannot add a base-less observation to a MAIRGraph");
  OptimizationConfig.initializeObservation(Obs);
  Obs.setMemoryObject(&getOrCreateMemoryObject(*Base));
}

void MAIRGraph::unregisterObservation(Observation &Obs) {
  Obs.clearMemoryObject();
}

ProgramPoint &MAIRGraph::createBBProgramPoint(const BasicBlock &BB,
                                              bool IsEntry) {
  assertLive("create a program point on");
  const Instruction *Anchor =
      IsEntry ? &*BB.getFirstNonPHIIt() : BB.getTerminator();
  assert(Anchor && "a basic-block program point must have an anchor");
  ProgramPoint &PP = addProgramPoint(allocateNode<ProgramPoint>(
      createProgramPointName(), *Anchor,
      IsEntry ? ProgramPoint::SiteKind::BasicBlockEntry
              : ProgramPoint::SiteKind::BasicBlockExit));
  (IsEntry ? BBEntryRepresentatives : BBExitRepresentatives)[&BB] = &PP;
  return PP;
}

ProgramPoint &MAIRGraph::createInstProgramPoint(const BasicBlock &BB,
                                                const Instruction &Anchor) {
  assertLive("create a program point on");
  assert(Anchor.getParent() == &BB &&
         "a memory-instruction program point anchor must belong to its block");
  auto &PP = addProgramPoint(allocateNode<ProgramPoint>(
      createProgramPointName(), Anchor, ProgramPoint::SiteKind::MemoryInst));
  const DataLayout &DL = Anchor.getModule()->getDataLayout();
  if (const auto *Transfer = dyn_cast<AnyMemTransferInst>(&Anchor)) {
    PP.addObservation(makeObservation(Anchor, *Transfer->getRawDest(),
                                      getAccessSize(*Transfer->getLength()),
                                      AccessKind::Write, DL));
    PP.addObservation(makeObservation(Anchor, *Transfer->getRawSource(),
                                      getAccessSize(*Transfer->getLength()),
                                      AccessKind::Read, DL));
  } else {
    PP.addObservation(Observation::fromInstruction(Anchor, DL));
  }
  return PP;
}

ProgramPoint &MAIRGraph::createProgramPoint(ProgramPointSite Site,
                                            ProgramPointGuard Guard) {
  assertLive("create a program point on");
  return addProgramPoint(allocateNode<ProgramPoint>(
      createProgramPointName(), std::move(Site), std::move(Guard)));
}

ProgramPoint &MAIRGraph::createEdgeProgramPoint(const BasicBlock &From,
                                                const BasicBlock &To,
                                                ProgramPointGuard Guard) {
  assertLive("create a program point on");
  return createProgramPoint(ProgramPointSite::edge(From, To), std::move(Guard));
}

MAIRScalarExpr MAIRGraph::createConstantExpr(Type &Ty, const APInt &Value) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  auto *Node = new (ScalarExprAllocator.Allocate()) MAIRScalarExprNode(
      *ScalarExprOwner, MAIRScalarExpr::Kind::Constant, Ty, Value, nullptr, {});
  return MAIRScalarExpr(*Node);
}

MAIRScalarExpr MAIRGraph::createValueExpr(const Value &Value) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  auto *Node = new (ScalarExprAllocator.Allocate())
      MAIRScalarExprNode(*ScalarExprOwner, MAIRScalarExpr::Kind::LLVMValue,
                         *Value.getType(), APInt(1, 0), &Value, {});
  return MAIRScalarExpr(*Node);
}

MAIRScalarExpr MAIRGraph::createUnaryExpr(MAIRScalarExpr::Kind Kind,
                                          Type &ResultType,
                                          MAIRScalarExpr Operand) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  MAIRScalarExpr Operands[] = {Operand};
  auto *Node = new (ScalarExprAllocator.Allocate()) MAIRScalarExprNode(
      *ScalarExprOwner, Kind, ResultType, APInt(1, 0), nullptr, Operands);
  return MAIRScalarExpr(*Node);
}

MAIRScalarExpr MAIRGraph::createBinaryExpr(MAIRScalarExpr::Kind Kind,
                                           Type &ResultType, MAIRScalarExpr LHS,
                                           MAIRScalarExpr RHS) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  MAIRScalarExpr Operands[] = {LHS, RHS};
  auto *Node = new (ScalarExprAllocator.Allocate()) MAIRScalarExprNode(
      *ScalarExprOwner, Kind, ResultType, APInt(1, 0), nullptr, Operands);
  return MAIRScalarExpr(*Node);
}

bool MAIRGraph::ownsScalarExpr(MAIRScalarExpr Expr) const {
  assertLive("query");
  return ScalarExprOwner && Expr.getOwner() == ScalarExprOwner.get();
}

MAIRGraph::iterator MAIRGraph::erase(ProgramPoint &PP) {
  assertLive("erase from");
  assert(PP.getParent() == this &&
         "ProgramPoint does not belong to this MAIRGraph");
  auto It = PP.getIterator();
  unsigned SelfEdges = 0;
  for (const ProgramPointEdge &Edge : PP.Children)
    SelfEdges += &Edge.getNode() == &PP;

  // Remove each peer's mirror entry directly. A self-edge occurs in both local
  // lists but contributes only once to the graph edge count.
  auto RemoveMirror = [](auto &Edges, const ProgramPoint &Neighbor,
                         PPEdgeKind Kind) {
    auto EdgeIt = llvm::find_if(Edges, [&](const ProgramPointEdge &Edge) {
      return &Edge.getNode() == &Neighbor && Edge.getKind() == Kind;
    });
    assert(EdgeIt != Edges.end() && "MAIR edge lists are not symmetric");
    Edges.erase(EdgeIt);
  };
  for (const ProgramPointEdge &Edge : PP.Children) {
    ProgramPoint &To = Edge.getNode();
    if (&To != &PP)
      RemoveMirror(To.Parents, PP, Edge.getKind());
  }
  for (const ProgramPointEdge &Edge : PP.Parents) {
    ProgramPoint &From = Edge.getNode();
    if (&From != &PP)
      RemoveMirror(From.Children, PP, Edge.getKind());
  }
  const unsigned IncidentEdges =
      PP.Children.size() + PP.Parents.size() - SelfEdges;
  PP.Children.clear();
  PP.Parents.clear();
  NumEdges -= IncidentEdges;

  SmallVector<const BasicBlock *, 4> EntryKeys;
  SmallVector<const BasicBlock *, 4> ExitKeys;
  for (const auto &Entry : BBEntryRepresentatives)
    if (Entry.second == &PP)
      EntryKeys.push_back(Entry.first);
  for (const auto &Entry : BBExitRepresentatives)
    if (Entry.second == &PP)
      ExitKeys.push_back(Entry.first);
  for (const BasicBlock *BB : EntryKeys)
    BBEntryRepresentatives.erase(BB);
  for (const BasicBlock *BB : ExitKeys)
    BBExitRepresentatives.erase(BB);
  for (Observation &Obs : PP.Observations)
    unregisterObservation(Obs);
  auto Next = ProgramPoints.erase(It);
  --NumProgramPoints;
  return Next;
}

void MAIRGraph::clearStorage() {
  GraphAllocator.DestroyAll();
  ScalarExprAllocator.DestroyAll();
  MemoryObjectAllocator.DestroyAll();
  ProgramPoints.clear();
  MemoryObjects.clear();
  BBEntryRepresentatives.clear();
  BBExitRepresentatives.clear();
  NumProgramPoints = 0;
  NumEdges = 0;
  NextProgramPointID = 0;
}

void MAIRGraph::clear() {
  assertLive("clear");
  clearStorage();
}

ProgramPoint *MAIRGraph::find(StringRef Name) {
  assertLive("find in");
  return const_cast<ProgramPoint *>(std::as_const(*this).find(Name));
}

const ProgramPoint *MAIRGraph::find(StringRef Name) const {
  assertLive("find in");
  for (const ProgramPoint &PP : ProgramPoints)
    if (PP.getName() == Name)
      return &PP;
  return nullptr;
}

ProgramPoint &MAIRGraph::getBBEntryRepresentative(const BasicBlock &BB) {
  assertLive("query");
  ProgramPoint *Representative = BBEntryRepresentatives.lookup(&BB);
  assert(Representative && "Seems the MAIRGraph is broken.");
  return *Representative;
}

const ProgramPoint &
MAIRGraph::getBBEntryRepresentative(const BasicBlock &BB) const {
  assertLive("query");
  const ProgramPoint *Representative = BBEntryRepresentatives.lookup(&BB);
  assert(Representative && "Seems the MAIRGraph is broken.");
  return *Representative;
}

ProgramPoint &MAIRGraph::getBBExitRepresentative(const BasicBlock &BB) {
  assertLive("query");
  ProgramPoint *Representative = BBExitRepresentatives.lookup(&BB);
  assert(Representative && "Seems the MAIRGraph is broken.");
  return *Representative;
}

const ProgramPoint &
MAIRGraph::getBBExitRepresentative(const BasicBlock &BB) const {
  assertLive("query");
  const ProgramPoint *Representative = BBExitRepresentatives.lookup(&BB);
  assert(Representative && "Seems the MAIRGraph is broken.");
  return *Representative;
}

void MAIRGraph::replaceProgramPoint(ProgramPoint &From, ProgramPoint &Into) {
  assertLive("replace program points in");
  assert(From.getParent() == this && Into.getParent() == this &&
         "replacement program points must belong to this graph");
  assert(Into.hasCompatibleMergeSemantics(From) &&
         "replacement program points have incompatible semantics");
  Into.mergeRepresentationFrom(From);
  for (auto &Entry : BBEntryRepresentatives)
    if (Entry.second == &From)
      Entry.second = &Into;
  for (auto &Entry : BBExitRepresentatives)
    if (Entry.second == &From)
      Entry.second = &Into;
}

void MAIRGraph::contractProgramPoint(ProgramPoint &Into, ProgramPoint &From) {
  assertLive("contract program points in");
  assert(Into.getParent() == this && From.getParent() == this &&
         "contracted program points must belong to this graph");
  assert(Into.hasCompatibleMergeSemantics(From) &&
         "contracted program points have incompatible semantics");
  assert(Into.hasChild(From, PPEdgeKind::PostDom) &&
         From.hasChild(Into, PPEdgeKind::Dom) &&
         "contraction requires mutual Dom/PostDom edges");

  Into.prependObservationsFrom(From);
  SmallVector<ProgramPointEdge, 2> Incoming(From.parents());
  SmallVector<ProgramPointEdge, 2> Outgoing(From.children());
  for (const ProgramPointEdge &Edge : Incoming) {
    ProgramPoint &Pred = Edge.getNode();
    if (&Pred != &Into)
      addEdge(Pred, Into, Edge.getKind());
  }
  for (const ProgramPointEdge &Edge : Outgoing) {
    ProgramPoint &Succ = Edge.getNode();
    if (&Succ != &Into)
      addEdge(Into, Succ, Edge.getKind());
  }

  replaceProgramPoint(From, Into);
  erase(From);
}

void MAIRGraph::addEdge(ProgramPoint &From, ProgramPoint &To, PPEdgeKind Kind) {
  assertLive("add an edge to");
  assert(From.getParent() == this && To.getParent() == this &&
         "MAIRGraph edge endpoints must belong to this graph");
  if (hasEdge(From, To, Kind))
    return;
  From.Children.push_back(ProgramPointEdge(To, Kind));
  To.Parents.push_back(ProgramPointEdge(From, Kind));
  ++NumEdges;
}

void MAIRGraph::removeEdge(ProgramPoint &From, ProgramPoint &To,
                           PPEdgeKind Kind) {
  assertLive("remove an edge from");
  assert(From.getParent() == this && To.getParent() == this &&
         "MAIRGraph edge endpoints must belong to this graph");
  auto ChildIt =
      llvm::find_if(From.Children, [&](const ProgramPointEdge &Edge) {
        return &Edge.getNode() == &To && Edge.getKind() == Kind;
      });
  if (ChildIt == From.Children.end())
    return;
  From.Children.erase(ChildIt);

  auto ParentIt = llvm::find_if(To.Parents, [&](const ProgramPointEdge &Edge) {
    return &Edge.getNode() == &From && Edge.getKind() == Kind;
  });
  assert(ParentIt != To.Parents.end() && "MAIR edge lists are not symmetric");
  To.Parents.erase(ParentIt);
  --NumEdges;
}

bool MAIRGraph::hasEdge(const ProgramPoint &From, const ProgramPoint &To,
                        PPEdgeKind Kind) const {
  assertLive("query");
  for (const ProgramPointEdge &Edge : From.children())
    if (&Edge.getNode() == &To && Edge.getKind() == Kind)
      return true;
  return false;
}

bool MAIRGraph::isAncestor(const ProgramPoint &Ancestor,
                           const ProgramPoint &Descendant,
                           PPEdgeKind Kind) const {
  assertLive("query ancestry in");
  assert(Ancestor.getParent() == this && Descendant.getParent() == this &&
         "ancestry endpoints must belong to this graph");
  if (&Ancestor == &Descendant)
    return false;

  SmallVector<const ProgramPoint *, 8> Worklist{&Ancestor};
  SmallPtrSet<const ProgramPoint *, 16> Visited;
  Visited.insert(&Ancestor);
  while (!Worklist.empty()) {
    const ProgramPoint &Current = *Worklist.pop_back_val();
    for (const ProgramPointEdge &Edge : Current.children()) {
      if (Edge.getKind() != Kind)
        continue;
      const ProgramPoint *Next = &Edge.getNode();
      if (Next == &Descendant)
        return true;
      if (Visited.insert(Next).second)
        Worklist.push_back(Next);
    }
  }
  return false;
}

MemoryObject *MAIRGraph::getMemoryObject(const Value &Base) {
  assertLive("query");
  return MemoryObjects.lookup(&Base);
}

const MemoryObject *MAIRGraph::getMemoryObject(const Value &Base) const {
  assertLive("query");
  return MemoryObjects.lookup(&Base);
}

std::string MAIRGraph::toString() const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS);
  OS.flush();
  return S;
}

void MAIRGraph::print(raw_ostream &OS) const {
  assertLive("print");
  OS << "mair graph";
  if (F) {
    OS << " of fn ";
    OS << getPrintedValueName(*F);
  }
  OS << " {\n";
  for (const ProgramPoint &PP : ProgramPoints) {
    PP.print(OS);
    OS << "\n";
  }
  OS << "}\n\n";
}

} // namespace mair
