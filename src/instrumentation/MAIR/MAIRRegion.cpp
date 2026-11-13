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
  if (Node->Kind == Kind::LLVMValue) {
    if (Node->LLVMValue)
      printValueName(OS, *Node->LLVMValue);
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

bool AbstractAccess::contains(const AbstractAccess &Other) const {
  if (*this == Other)
    return true;
  if (Base != Other.Base)
    return false;
  if (!Offset.isConstant() || !Size.isConstant() ||
      !Other.Offset.isConstant() || !Other.Size.isConstant())
    return false;

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

} // namespace

std::optional<AbstractAccess> AbstractAccess::merge(const AbstractAccess &A,
                                                    const AbstractAccess &B,
                                                    const DataLayout *DL,
                                                    ScalarEvolution *SE,
                                                    MAIRGraph *ExprGraph) {
  if (A == B)
    return A;
  if (A.Base != B.Base)
    return std::nullopt;
  if (A.contains(B))
    return A;
  if (B.contains(A))
    return B;
  if (A.hasSizeExpr() || B.hasSizeExpr())
    return mergeExpressionSizedRanges(A, B, ExprGraph);
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

    const AbstractAccess &First = BBegin.slt(Zero) ? B : A;
    return AbstractAccess(First.Base, First.Offset,
                          AbstractValue::constant(MergedSize.getZExtValue()));
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

  return AbstractAccess(A.Base, AbstractValue::constant(Begin),
                        AbstractValue::constant(End - Begin));
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
    printValueName(OS, *DynamicMask);
    return;
  }
  OS << "<";
  ListSeparator Separator(",");
  for (unsigned Lane = 0; Lane < ConstantMask.size(); ++Lane)
    OS << Separator << (ConstantMask.test(Lane) ? '1' : '0');
  OS << ">";
}

} // namespace mair
