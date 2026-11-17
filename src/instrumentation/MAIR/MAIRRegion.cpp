#include "MAIR/MAIR.h"

#include "MAIR/MAIRInternal.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

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

const BasicBlock *MAIRScalarExpr::getLoopHeader() const {
  assert(Node && Node->Kind == Kind::BackedgeCount &&
         "MAIR scalar expression is not a backedge count");
  return cast<BasicBlock>(Node->LLVMValue);
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
      Node->LLVMValue != Other.Node->LLVMValue)
    return false;
  if (Node->Kind == Kind::Constant && Node->Constant != Other.Node->Constant)
    return false;
  return llvm::equal(Node->Operands, Other.Node->Operands);
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
  if (Node->Kind == Kind::LLVMValue || Node->Kind == Kind::BackedgeCount) {
    if (Node->Kind == Kind::BackedgeCount)
      OS << "backedges(";
    if (Node->LLVMValue)
      printValueName(OS, *Node->LLVMValue);
    else
      OS << "<null>";
    if (Node->Kind == Kind::BackedgeCount)
      OS << ")";
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
  case Kind::PtrToInt:
    OS << "ptrtoint(";
    break;
  case Kind::Constant:
  case Kind::LLVMValue:
  case Kind::BackedgeCount:
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
  if (Alignment > 1)
    OS << ", align " << Alignment.value();
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

bool isValueAvailableAt(const Value &V, const Instruction &At,
                        const DominatorTree &DT) {
  // Constants and arguments dominate every instruction. Other values, such
  // as blocks or metadata, are never operands of an observation.
  return isa<Instruction, Argument, Constant>(V) && DT.dominates(&V, &At);
}

bool isValueAvailableAt(const AbstractValue &V, const Instruction &At,
                        const DominatorTree &DT) {
  if (V.isConstant())
    return true;
  return V.isSymbol() && isValueAvailableAt(*V.getSymbol(), At, DT);
}

const Value *getByteOffsetIndex(const Value &Ptr, const Value &Base,
                                const DataLayout &DL) {
  const auto *GEP = dyn_cast<GEPOperator>(&Ptr);
  if (!GEP || GEP->getNumIndices() != 1 ||
      GEP->getPointerOperand()->stripPointerCasts() != &Base)
    return nullptr;
  TypeSize Stride = DL.getTypeAllocSize(GEP->getSourceElementType());
  if (!Stride.isFixed() || Stride.getFixedValue() != 1)
    return nullptr;
  return GEP->getOperand(1);
}

namespace {
AbstractAccess accessAtPointer(const Value &Ptr, AbstractValue Size,
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

  // gep i8 p, %i has byte offset %i. The GEP itself is not the offset: %i and
  // %i+1 are symbolic byte offsets of the same object.
  if (const Value *Index = getByteOffsetIndex(Ptr, *Underlying, DL)) {
    if (const auto *ConstIndex = dyn_cast<ConstantInt>(Index)) {
      if (std::optional<uint64_t> IndexOffset =
              getNonNegativeU64(ConstIndex->getValue()))
        return AbstractAccess(Underlying, AbstractValue::constant(*IndexOffset),
                              Size);
    } else if (Index->getType()->isIntegerTy()) {
      return AbstractAccess(Underlying, AbstractValue::symbol(*Index), Size);
    }
  }

  if (Underlying != &Ptr)
    return AbstractAccess(Underlying, AbstractValue::symbol(Ptr), Size);

  return AbstractAccess(&Ptr, AbstractValue::constant(0), Size);
}
} // namespace

AbstractAccess AbstractAccess::fromPointerAndSize(const Value &Ptr,
                                                  AbstractValue Size,
                                                  const DataLayout &DL,
                                                  Align Alignment) {
  // Each form accessAtPointer builds starts at Ptr itself.
  AbstractAccess Result = accessAtPointer(Ptr, std::move(Size), DL);
  Result.setAlign(Alignment);
  return Result;
}

namespace {
std::optional<int64_t> constantAddressDelta(const AbstractAccess &A,
                                            const AbstractAccess &B,
                                            const DataLayout *DL,
                                            ScalarEvolution *SE);
} // namespace

bool AbstractAccess::contains(const AbstractAccess &Other,
                              ScalarEvolution *SE) const {
  if (*this == Other)
    return true;
  if (Base != Other.Base)
    return false;
  // Equal lengths computed for different loops are separate expression
  // nodes, so compare their structure here. operator== keeps comparing node
  // identity, which merging and motion use to pick a result.
  if (hasSizeExpr() || Other.hasSizeExpr())
    return hasSizeExpr() && Other.hasSizeExpr() && Offset == Other.Offset &&
           SizeExpr == Other.SizeExpr;
  if (!Size.isConstant() || !Other.Size.isConstant())
    return false;
  if (!Offset.isConstant() || !Other.Offset.isConstant()) {
    // Other starts Delta bytes past this start.
    std::optional<int64_t> Delta;
    if (Offset.isSymbol() && Offset == Other.Offset)
      Delta = 0;
    else if (SE)
      Delta = constantAddressDelta(*this, Other, /*DL=*/nullptr, SE);
    if (!Delta || *Delta < 0)
      return false;
    APInt OtherEnd = APInt(128, *Delta) + APInt(128, Other.Size.getConstant());
    return OtherEnd.ule(APInt(128, Size.getConstant()));
  }

  uint64_t ThisBegin = Offset.getConstant();
  std::optional<uint64_t> ThisEnd =
      checkedAddUnsigned(ThisBegin, Size.getConstant());
  uint64_t OtherBegin = Other.Offset.getConstant();
  std::optional<uint64_t> OtherEnd =
      checkedAddUnsigned(OtherBegin, Other.Size.getConstant());
  if (!ThisEnd || !OtherEnd)
    return false;
  return ThisBegin <= OtherBegin && *OtherEnd <= *ThisEnd;
}

const SCEV *AbstractAccess::getAddressSCEV(ScalarEvolution &SE,
                                           ZeroOffsetBase ZeroOffset) const {
  if (!Base || !Base->getType()->isPointerTy())
    return nullptr;
  Type *ByteTy = Type::getInt8Ty(Base->getContext());
  Value *BaseValue = const_cast<Value *>(Base);
  if (Offset.isConstant()) {
    if (Offset.getConstant() == 0)
      return ZeroOffset == ZeroOffsetBase::Opaque ? SE.getUnknown(BaseValue)
                                                  : SE.getSCEV(BaseValue);
    Type *IndexTy = SE.getEffectiveSCEVType(Base->getType());
    unsigned Width = cast<IntegerType>(IndexTy)->getBitWidth();
    if (Width < 64 && (Offset.getConstant() >> Width) != 0)
      return nullptr;
    const SCEV *Index = SE.getConstant(IndexTy, Offset.getConstant(), true);
    return SE.getGEPExpr(SE.getUnknown(BaseValue), Index, ByteTy);
  }
  if (!Offset.isSymbol())
    return nullptr;
  // A pointer-typed symbol is the complete address, not an offset.
  Value *Symbol = const_cast<Value *>(Offset.getSymbol());
  if (Symbol->getType()->isPointerTy())
    return SE.getSCEV(Symbol);
  if (!Symbol->getType()->isIntegerTy())
    return nullptr;
  return SE.getGEPExpr(SE.getUnknown(BaseValue), SE.getSCEV(Symbol), ByteTy);
}

namespace {

ConstantRange expressionRange(const MAIRScalarExpr &Expr) {
  unsigned Bits = Expr.getType()->getIntegerBitWidth();
  using Kind = MAIRScalarExpr::Kind;
  switch (Expr.getKind()) {
  case Kind::Constant:
    return ConstantRange(Expr.getConstant());
  case Kind::ZeroExtend:
    return expressionRange(Expr.operands().front()).zeroExtend(Bits);
  case Kind::SignExtend:
    return expressionRange(Expr.operands().front()).signExtend(Bits);
  case Kind::Truncate:
    return expressionRange(Expr.operands().front()).truncate(Bits);
  case Kind::Add:
  case Kind::Mul: {
    ConstantRange Result(APInt(Bits, Expr.getKind() == Kind::Mul ? 1 : 0));
    for (const MAIRScalarExpr &Operand : Expr.operands()) {
      ConstantRange OperandRange = expressionRange(Operand);
      Result = Expr.getKind() == Kind::Mul ? Result.multiply(OperandRange)
                                           : Result.add(OperandRange);
    }
    return Result;
  }
  default:
    return ConstantRange::getFull(Bits);
  }
}

std::optional<int64_t> constantAddressDelta(const AbstractAccess &A,
                                            const AbstractAccess &B,
                                            const DataLayout *DL,
                                            ScalarEvolution *SE) {
  if (SE) {
    const SCEV *AddrA =
        A.getAddressSCEV(*SE, AbstractAccess::ZeroOffsetBase::Opaque);
    const SCEV *AddrB =
        B.getAddressSCEV(*SE, AbstractAccess::ZeroOffsetBase::Opaque);
    if (AddrA && AddrB && AddrA->getType() == AddrB->getType()) {
      if (std::optional<APInt> Diff =
              SE->computeConstantDifference(AddrB, AddrA))
        if (Diff->isSignedIntN(64))
          return Diff->getSExtValue();
    }
    return std::nullopt;
  }
  if (!DL || !A.getBase() || !A.getBase()->getType()->isPointerTy())
    return std::nullopt;
  unsigned IndexBits = DL->getIndexTypeSizeInBits(A.getBase()->getType());
  auto StripOffset = [&](const AbstractAccess &Access,
                         APInt &Offset) -> std::optional<AbstractValue> {
    const AbstractValue &Start = Access.getOffset();
    if (Start.isUnknown())
      return std::nullopt;
    if (!Start.isSymbol() || !Start.getSymbol()->getType()->isPointerTy())
      return Start;
    const Value *Pointer = Start.getSymbol();
    if (Pointer->getType() != Access.getBase()->getType())
      return std::nullopt;
    const Value *Stripped = Pointer->stripAndAccumulateConstantOffsets(
        *DL, Offset, /*AllowNonInbounds=*/true);
    if (Stripped->getType() != Pointer->getType())
      return std::nullopt;
    AbstractAccess Anchor =
        AbstractAccess::fromPointerAndSize(*Stripped, Access.getSize(), *DL);
    if (Anchor.getBase() != Access.getBase())
      return std::nullopt;
    return Anchor.getOffset();
  };
  APInt OffsetA(IndexBits, 0), OffsetB(IndexBits, 0);
  auto AnchorA = StripOffset(A, OffsetA);
  auto AnchorB = StripOffset(B, OffsetB);
  if (!AnchorA || !AnchorB || *AnchorA != *AnchorB)
    return std::nullopt;
  APInt Difference = OffsetB.sext(IndexBits + 1) - OffsetA.sext(IndexBits + 1);
  return Difference.trySExtValue();
}

std::optional<AbstractAccess>
mergeExpressionSizedRanges(const AbstractAccess &A, const AbstractAccess &B,
                           MAIRGraph *ExprGraph) {
  if (!ExprGraph || !A.hasSizeExpr() || !B.hasSizeExpr() ||
      A.getSizeExpr() != B.getSizeExpr() || !A.getOffset().isConstant() ||
      !B.getOffset().isConstant())
    return std::nullopt;
  uint64_t ABegin = A.getOffset().getConstant();
  uint64_t BBegin = B.getOffset().getConstant();
  if (ABegin == BBegin)
    return A;
  uint64_t Low = std::min(ABegin, BBegin);
  uint64_t High = std::max(ABegin, BBegin);
  uint64_t Delta = High - Low;
  MAIRScalarExpr SizeExpr = A.getSizeExpr();
  Type *SizeTy = SizeExpr.getType();
  if (!SizeTy || !SizeTy->isIntegerTy())
    return std::nullopt;
  unsigned Bits = SizeTy->getIntegerBitWidth();
  unsigned ScratchBits = std::max(Bits, 64u) + 1;
  ConstantRange SizeRange = expressionRange(SizeExpr);
  APInt ExtraSize(ScratchBits, Delta);
  if (SizeRange.getUnsignedMin().zext(ScratchBits).ult(ExtraSize))
    return std::nullopt;
  APInt MaxMerged = SizeRange.getUnsignedMax().zext(ScratchBits) + ExtraSize;
  unsigned MergedBits = std::max(Bits, MaxMerged.getActiveBits());
  if (MergedBits > IntegerType::MAX_INT_BITS)
    return std::nullopt;
  if (MergedBits > Bits) {
    SizeTy = IntegerType::get(SizeTy->getContext(), MergedBits);
    SizeExpr = ExprGraph->createUnaryExpr(MAIRScalarExpr::Kind::ZeroExtend,
                                          *SizeTy, SizeExpr);
  }
  MAIRScalarExpr Extra =
      ExprGraph->createConstantExpr(*SizeTy, APInt(MergedBits, Delta));
  MAIRScalarExpr Merged = ExprGraph->createBinaryExpr(MAIRScalarExpr::Kind::Add,
                                                      *SizeTy, SizeExpr, Extra);
  AbstractAccess Result(A.getBase(), AbstractValue::constant(Low),
                        AbstractValue());
  Result.setSizeExpr(Merged);
  return Result;
}

/// A pointer start expression split as Root + (Constant + Terms...), where
/// the integer offset is a tree of adds.
struct PeriodicStart {
  MAIRScalarExpr Root;
  SmallVector<MAIRScalarExpr, 4> Terms;
  std::optional<APInt> Constant;
  Type *OffsetType = nullptr;
};

void collectAddTerms(MAIRScalarExpr Expr, PeriodicStart &Start) {
  using Kind = MAIRScalarExpr::Kind;
  if (Expr.getKind() == Kind::Add) {
    for (MAIRScalarExpr Operand : Expr.operands())
      collectAddTerms(Operand, Start);
    return;
  }
  if (Expr.getKind() == Kind::Constant) {
    Start.Constant = Start.Constant ? *Start.Constant + Expr.getConstant()
                                    : Expr.getConstant();
    return;
  }
  Start.Terms.push_back(Expr);
}

PeriodicStart splitPeriodicStart(MAIRScalarExpr Start) {
  PeriodicStart Result;
  if (Start.getKind() != MAIRScalarExpr::Kind::Add ||
      !Start.getType()->isPointerTy()) {
    Result.Root = Start;
    return Result;
  }
  // A pointer add is a pointer plus an integer offset (MAIRVerifier).
  Result.Root = Start.operands()[0];
  MAIRScalarExpr Offset = Start.operands()[1];
  Result.OffsetType = Offset.getType();
  collectAddTerms(Offset, Result);
  return Result;
}

/// B.Start - A.Start in bytes, if the starts differ only by constants. The
/// offsets are lowered as i8 GEP indices, so they must be as wide as the
/// pointer index for the difference to be exact.
std::optional<int64_t> constantStartDelta(const PeriodicRegion &A,
                                          const PeriodicRegion &B,
                                          const DataLayout &DL) {
  Type *PointerType = A.getStart().getType();
  if (!PointerType || !PointerType->isPointerTy() ||
      B.getStart().getType() != PointerType)
    return std::nullopt;
  const unsigned IndexBits = DL.getIndexTypeSizeInBits(PointerType);
  PeriodicStart SA = splitPeriodicStart(A.getStart());
  PeriodicStart SB = splitPeriodicStart(B.getStart());
  if (SA.Root != SB.Root || !llvm::equal(SA.Terms, SB.Terms))
    return std::nullopt;
  for (const PeriodicStart *S : {&SA, &SB})
    if (S->OffsetType && S->OffsetType->getIntegerBitWidth() != IndexBits)
      return std::nullopt;
  APInt CA = SA.Constant.value_or(APInt(IndexBits, 0));
  APInt CB = SB.Constant.value_or(APInt(IndexBits, 0));
  return (CB.sext(IndexBits + 1) - CA.sext(IndexBits + 1)).trySExtValue();
}

uint64_t absValue(int64_t V) {
  return V < 0 ? -static_cast<uint64_t>(V) : static_cast<uint64_t>(V);
}

} // namespace

std::optional<PeriodicRegion> PeriodicRegion::merge(const PeriodicRegion &A,
                                                    const PeriodicRegion &B,
                                                    const DataLayout *DL,
                                                    MAIRGraph *ExprGraph) {
  using Kind = MAIRScalarExpr::Kind;
  if (A == B) {
    PeriodicRegion Result = A;
    Result.Alignment = std::max(A.Alignment, B.Alignment);
    return Result;
  }
  if (!DL || !ExprGraph || A.Base != B.Base || A.Stride != B.Stride ||
      A.TripCount != B.TripCount || A.AccessSize.getKind() != Kind::Constant ||
      B.AccessSize.getKind() != Kind::Constant ||
      A.AccessSize.getType() != B.AccessSize.getType())
    return std::nullopt;
  for (const PeriodicRegion *R : {&A, &B})
    for (MAIRScalarExpr E : {R->Start, R->Stride, R->AccessSize, R->TripCount})
      if (!ExprGraph->ownsScalarExpr(E))
        return std::nullopt;
  std::optional<int64_t> Delta = constantStartDelta(A, B, *DL);
  if (!Delta)
    return std::nullopt;

  const PeriodicRegion &Low = *Delta < 0 ? B : A;
  const PeriodicRegion &High = *Delta < 0 ? A : B;
  APInt Distance = APInt(128, static_cast<uint64_t>(*Delta), true).abs();
  APInt LowSize = Low.AccessSize.getConstant().zext(128);
  APInt HighEnd = Distance + High.AccessSize.getConstant().zext(128);
  // A gap between the two footprints of one trip is not covered.
  if (Distance.ugt(LowSize))
    return std::nullopt;
  // Each trip of the union starts where the lower region's trip does, and
  // the higher region's trip starts Distance bytes past it.
  Align Alignment = std::max(
      Low.Alignment, commonAlignment(High.Alignment, Distance.getZExtValue()));
  if (HighEnd.ule(LowSize)) {
    PeriodicRegion Result = Low;
    Result.Alignment = Alignment;
    return Result;
  }
  Type *SizeType = Low.AccessSize.getType();
  if (HighEnd.getActiveBits() > SizeType->getIntegerBitWidth())
    return std::nullopt;
  MAIRScalarExpr Size = ExprGraph->createConstantExpr(
      *SizeType, HighEnd.trunc(SizeType->getIntegerBitWidth()));
  return PeriodicRegion(Low.Base, Low.Start, Low.Stride, Size, Low.TripCount,
                        Alignment);
}

std::optional<int64_t> PeriodicRegion::getInterleaveDistance(
    const PeriodicRegion &A, const PeriodicRegion &B, const DataLayout &DL) {
  using Kind = MAIRScalarExpr::Kind;
  if (A.Base != B.Base || A.Stride != B.Stride || A.TripCount != B.TripCount ||
      A.AccessSize != B.AccessSize || A.Stride.getKind() != Kind::Constant ||
      A.AccessSize.getKind() != Kind::Constant)
    return std::nullopt;
  std::optional<int64_t> Delta = constantStartDelta(A, B, DL);
  // |S| must fit in 63 bits for mergeInterleaved's arithmetic.
  const APInt &Stride = A.Stride.getConstant();
  if (!Delta || *Delta == 0 || Stride.getSignificantBits() > 63 ||
      absValue(*Delta) >= absValue(Stride.getSExtValue()))
    return std::nullopt;
  return Delta;
}

std::optional<PeriodicRegion>
PeriodicRegion::mergeInterleaved(ArrayRef<const PeriodicRegion *> Slices,
                                 const DataLayout *DL, MAIRGraph *ExprGraph) {
  using Kind = MAIRScalarExpr::Kind;
  const uint64_t M = Slices.size();
  if (M < 2 || !DL || !ExprGraph)
    return std::nullopt;
  for (const PeriodicRegion *R : Slices)
    for (MAIRScalarExpr E : {R->Start, R->Stride, R->AccessSize, R->TripCount})
      if (!ExprGraph->ownsScalarExpr(E))
        return std::nullopt;

  // The starts by distance from the first slice. Each distance is less than
  // |S| < 2^63, so the arithmetic below cannot overflow.
  const PeriodicRegion &First = *Slices.front();
  SmallVector<std::pair<int64_t, const PeriodicRegion *>, 4> Starts;
  Starts.push_back({0, &First});
  for (const PeriodicRegion *R : Slices.drop_front()) {
    std::optional<int64_t> Delta = getInterleaveDistance(First, *R, *DL);
    if (!Delta)
      return std::nullopt;
    Starts.push_back({*Delta, R});
  }
  auto *TripType = dyn_cast<IntegerType>(First.TripCount.getType());
  const int64_t S = First.Stride.getConstant().getSExtValue();
  if (!TripType || absValue(S) % M != 0)
    return std::nullopt;
  const int64_t Gap = absValue(S) / M;
  llvm::sort(Starts, llvm::less_first());
  for (uint64_t K = 0; K < M; ++K)
    if (Starts[K].first != Starts.front().first + static_cast<int64_t>(K) * Gap)
      return std::nullopt;

  MAIRScalarExpr Start =
      S > 0 ? Starts.front().second->Start : Starts.back().second->Start;
  Type &StrideType = *First.Stride.getType();
  MAIRScalarExpr Stride = ExprGraph->createConstantExpr(
      StrideType, APInt(StrideType.getIntegerBitWidth(),
                        S / static_cast<int64_t>(M), /*isSigned=*/true));

  // m * T fits in ceil(log2 m) more bits than T.
  const unsigned Width = TripType->getBitWidth();
  auto *WideType =
      IntegerType::get(TripType->getContext(), Width + Log2_64_Ceil(M));
  MAIRScalarExpr Trips;
  if (First.TripCount.getKind() == Kind::Constant) {
    APInt Product =
        First.TripCount.getConstant().zext(WideType->getBitWidth()) * M;
    Trips = Product.getActiveBits() <= Width
                ? ExprGraph->createConstantExpr(*TripType, Product.trunc(Width))
                : ExprGraph->createConstantExpr(*WideType, Product);
  } else {
    Trips = ExprGraph->createBinaryExpr(
        Kind::Mul, *WideType,
        ExprGraph->createConstantExpr(*WideType,
                                      APInt(WideType->getBitWidth(), M)),
        ExprGraph->createUnaryExpr(Kind::ZeroExtend, *WideType,
                                   First.TripCount));
  }
  // Every trip of the union is a trip of some slice.
  Align Alignment = First.Alignment;
  for (const PeriodicRegion *R : Slices)
    Alignment = std::min(Alignment, R->Alignment);
  return PeriodicRegion(First.Base, Start, Stride, First.AccessSize, Trips,
                        Alignment);
}

namespace {
/// How many bytes \p Part starts past \p Start, if their offsets tell.
std::optional<uint64_t> startDistance(const AbstractAccess &Start,
                                      const AbstractAccess &Part) {
  const AbstractValue &From = Start.getOffset();
  const AbstractValue &To = Part.getOffset();
  if (From.isUnknown() || To.isUnknown())
    return std::nullopt;
  if (From == To)
    return 0;
  if (From.isConstant() && To.isConstant() &&
      To.getConstant() >= From.getConstant())
    return To.getConstant() - From.getConstant();
  return std::nullopt;
}

/// Strengthen the alignment known for the start of \p Result with what
/// \p Part proves: an address aligned to A lies \p Distance bytes past the
/// start, so the start is aligned to what A and Distance have in common.
void proveStartAlign(AbstractAccess &Result, const AbstractAccess &Part,
                     uint64_t Distance) {
  Result.setAlign(
      std::max(Result.getAlign(), commonAlignment(Part.getAlign(), Distance)));
}
} // namespace

std::optional<AbstractAccess> AbstractAccess::merge(const AbstractAccess &A,
                                                    const AbstractAccess &B,
                                                    const DataLayout *DL,
                                                    ScalarEvolution *SE,
                                                    MAIRGraph *ExprGraph) {
  // Each union below starts where A or B starts, so it knows that one's
  // alignment, and the other proves more if its distance is known.
  auto WithStartAlign = [&](AbstractAccess Result) {
    for (const AbstractAccess *Part : {&A, &B})
      if (std::optional<uint64_t> Distance = startDistance(Result, *Part))
        proveStartAlign(Result, *Part, *Distance);
    return Result;
  };
  if (A == B)
    return WithStartAlign(A);
  if (A.Base != B.Base)
    return std::nullopt;
  if (A.contains(B))
    return WithStartAlign(A);
  if (B.contains(A))
    return WithStartAlign(B);
  if (A.hasSizeExpr() || B.hasSizeExpr()) {
    std::optional<AbstractAccess> Merged =
        mergeExpressionSizedRanges(A, B, ExprGraph);
    if (!Merged)
      return std::nullopt;
    return WithStartAlign(*Merged);
  }
  if (!A.Size.isConstant() || !B.Size.isConstant())
    return std::nullopt;

  if (!A.Offset.isConstant() || !B.Offset.isConstant()) {
    std::optional<int64_t> PointerDelta = constantAddressDelta(A, B, DL, SE);
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

    // The offsets are not comparable, so the distance of the later part is
    // the pointer delta.
    const bool BFirst = BBegin.slt(Zero);
    const AbstractAccess &First = BFirst ? B : A;
    AbstractAccess Result(First.Base, First.Offset,
                          AbstractValue::constant(MergedSize.getZExtValue()),
                          First.Alignment);
    proveStartAlign(Result, BFirst ? A : B,
                    static_cast<uint64_t>(BBegin.abs().getZExtValue()));
    return Result;
  }

  uint64_t ABegin = A.Offset.getConstant();
  std::optional<uint64_t> AEnd =
      checkedAddUnsigned(ABegin, A.Size.getConstant());
  uint64_t BBegin = B.Offset.getConstant();
  std::optional<uint64_t> BEnd =
      checkedAddUnsigned(BBegin, B.Size.getConstant());
  if (!AEnd || !BEnd)
    return std::nullopt;
  if (*AEnd < BBegin || *BEnd < ABegin)
    return std::nullopt;

  uint64_t Begin = std::min(ABegin, BBegin);
  uint64_t End = std::max(*AEnd, *BEnd);

  return WithStartAlign(AbstractAccess(A.Base, AbstractValue::constant(Begin),
                                       AbstractValue::constant(End - Begin)));
}

void AbstractAccess::print(raw_ostream &OS) const {
  if (!Base && Offset.isUnknown()) {
    OS << "<unknown>";
    return;
  }
  if (Base)
    printValueName(OS, *Base);
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

/// Whether \p A and \p B mask the same lanes of the same bounds, so that the
/// masks alone decide containment.
static bool haveComparableLanes(const MaskedRegion &A, const MaskedRegion &B) {
  return A.isComparable() && B.isComparable() &&
         A.getBounds() == B.getBounds() &&
         A.getElementSize() == B.getElementSize() &&
         A.getNumLanes() == B.getNumLanes();
}

bool MaskedRegion::contains(const MaskedRegion &Other) const {
  if (!haveComparableLanes(*this, Other))
    return false;
  if (!IsConstant || !Other.IsConstant)
    return !IsConstant && !Other.IsConstant && DynamicMask == Other.DynamicMask;
  return Other.ConstantMask.subsetOf(ConstantMask);
}

std::optional<MaskedRegion> MaskedRegion::merge(const MaskedRegion &A,
                                                const MaskedRegion &B) {
  if (!haveComparableLanes(A, B))
    return std::nullopt;
  if (!A.IsConstant || !B.IsConstant) {
    if (!A.IsConstant && !B.IsConstant && A.DynamicMask == B.DynamicMask)
      return A;
    return std::nullopt;
  }
  SmallBitVector Combined = A.ConstantMask;
  Combined |= B.ConstantMask;
  // Comparable lanes have equal bounds, so both alignments hold.
  AbstractAccess Bounds = A.Bounds;
  Bounds.setAlign(std::max(A.Bounds.getAlign(), B.Bounds.getAlign()));
  return MaskedRegion(std::move(Bounds), std::move(Combined), A.ElementSize);
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
    printValueName(OS, *DynamicMask);
  } else {
    OS << "<";
    ListSeparator Separator(",");
    for (unsigned Lane = 0; Lane < ConstantMask.size(); ++Lane)
      OS << Separator << (ConstantMask.test(Lane) ? '1' : '0');
    OS << ">";
  }
  if (Bounds.getAlign() > 1)
    OS << ", align " << Bounds.getAlign().value();
}

} // namespace mair
