#pragma once

// Declarations shared by the MAIR core sources (MAIR.cpp, MAIRGraphBuilder.cpp,
// MAIRObservation.cpp, MAIRRegion.cpp). Nothing else includes this file.

#include "MAIR/MAIR.h"

#include <optional>
#include <string>

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

struct MAIRGraphResources {
  SpecificBumpPtrAllocator<MemoryObject> MemoryObjectAllocator;
  MAIRScalarExprOwner ScalarExprOwner;
  SpecificBumpPtrAllocator<MAIRScalarExprNode> ScalarExprAllocator;
  DenseMap<const Value *, MemoryObject *> MemoryObjects;
  unsigned NextProgramPointID = 0;
};

inline void printValueName(raw_ostream &OS, const Value &V) {
  V.printAsOperand(OS, /*PrintType=*/false);
}

template <class T> std::string printToString(const T &Object) {
  std::string Text;
  raw_string_ostream OS(Text);
  Object.print(OS);
  return Text;
}

inline std::optional<uint64_t> getNonNegativeU64(const APInt &Value) {
  if (Value.isNegative() || Value.getActiveBits() > 64)
    return std::nullopt;
  return Value.getZExtValue();
}

inline void printAbstractValue(raw_ostream &OS, const AbstractValue &Value) {
  if (Value.isUnknown())
    OS << "unknown";
  else if (Value.isConstant())
    OS << Value.getConstant();
  else
    printValueName(OS, *Value.getSymbol());
}

bool isMaskedMemoryIntrinsic(const Instruction &I);

/// A constant length in bytes, or the length value itself as a symbol.
AbstractValue getAccessSize(const Value &Value);

Observation makeObservation(const Instruction &I, const Value &Ptr,
                            AbstractValue Size, AccessKind Kind,
                            const DataLayout &DL);

} // namespace mair
