#pragma once

// Declarations shared by the MAIR core sources (MAIR.cpp, MAIRDump.cpp,
// MAIRGraphBuilder.cpp, MAIRObservation.cpp, MAIRRegion.cpp). Nothing else
// includes this file.

#include "MAIR/MAIR.h"

#include "llvm/IR/Metadata.h"
#include "llvm/IR/ModuleSlotTracker.h"

#include <optional>
#include <string>

namespace mair {

using llvm::MetadataAsValue;
using llvm::ModuleSlotTracker;

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

/// The slot tracker the dump functions (MAIRDump.cpp) install while they
/// print. Without one, printing an unnamed value such as %5 numbers the whole
/// module again, which makes dumping a large module quadratic.
class DumpSlotTrackerScope {
  inline static thread_local ModuleSlotTracker *Current = nullptr;
  ModuleSlotTracker *Previous;

public:
  explicit DumpSlotTrackerScope(ModuleSlotTracker &MST) : Previous(Current) {
    Current = &MST;
  }
  ~DumpSlotTrackerScope() { Current = Previous; }
  DumpSlotTrackerScope(const DumpSlotTrackerScope &) = delete;
  DumpSlotTrackerScope &operator=(const DumpSlotTrackerScope &) = delete;

  static ModuleSlotTracker *get() { return Current; }
};

inline void printValueName(raw_ostream &OS, const Value &V) {
  // Metadata is numbered differently when the tracker has not numbered all of
  // it, so it keeps the default path.
  ModuleSlotTracker *MST = DumpSlotTrackerScope::get();
  if (MST && !llvm::isa<MetadataAsValue>(V))
    V.printAsOperand(OS, /*PrintType=*/false, *MST);
  else
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

/// The observation of \p I accessing \p Size bytes at \p Ptr, which \p I
/// declares aligned to \p Alignment.
Observation makeObservation(const Instruction &I, const Value &Ptr,
                            AbstractValue Size, AccessKind Kind,
                            const DataLayout &DL, Align Alignment);

} // namespace mair
