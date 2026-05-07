#include "UbsanUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include "ValueUtils.h"
#include "MetaDataUtils.h"

using namespace llvm;

namespace {
constexpr const char *UbsanInterfacePrefix = "__ubsan_";
constexpr const char *UbsanReportPrefix = "__ubsan_handle_";
} // namespace

namespace __xsan {

// E.g., __ubsan_handle_add_overflow
bool isUbsanInterfaceCall(const Instruction &I) {
  if (!isa<CallBase>(I)) {
    return false;
  }
  if (!isNoSanitize(I)) {
    return false;
  }
  auto &CB = cast<CallBase>(I);
  auto *CalledFunction = CB.getCalledFunction();
  return CalledFunction && isUbsanFunction(*CalledFunction);
}

bool isUbsanReportCall(const Instruction &I) {
  if (!isUbsanInterfaceCall(I)) {
    return false;
  }
  auto &CB = cast<CallBase>(I);
  auto *CalledFunction = CB.getCalledFunction();
  return CalledFunction && isUbsanReportFunction(*CalledFunction);
}

bool isUbsanFunction(const Function &F) {
  return compat::startsWith(F.getName(), UbsanInterfacePrefix);
}

bool isUbsanReportFunction(const Function &F) {
  return compat::startsWith(F.getName(), UbsanReportPrefix);
}

bool hasUbsanFunction(const Module &M) {
  return any_of(M.functions(), isUbsanFunction);
}

/*
Match with the following pattern:
```
  %2 = tail call { i32, i1 } @llvm.sadd.with.overflow.i32(i32 %0, i32 1),
!nosanitize !5 %3 = extractvalue { i32, i1 } %2, 1, !nosanitize !5 br i1 %3,
label %4, label %6, !prof !6, !nosanitize !5

4:                                                ; preds = %1
  %5 = zext i32 %0 to i64, !nosanitize !5
  tail call void @__ubsan_handle_add_overflow(ptr nonnull @1, i64 %5, i64 1) #3,
!nosanitize !5 br label %6, !nosanitize !5

6:                                                ; preds = %4, %1
  %7 = extractvalue { i32, i1 } %2, 0, !nosanitize !5
  ret i32 %7
```
*/
bool isCheckedIntegerOverflowIntrinsicCall(llvm::Instruction &I) {
  static constexpr Intrinsic::ID ValidIDs[] = {
      Intrinsic::sadd_with_overflow, Intrinsic::uadd_with_overflow,
      Intrinsic::ssub_with_overflow, Intrinsic::usub_with_overflow};

  if (!isNoSanitize(I)) {
    return false;
  }

  // Check if the call is a checked integer overflow intrinsic call.
  // E.g, llvm.sadd.with.overflow.i32, llvm.uadd.with.overflow.i32, etc.
  // UBSan/Integer Sanitizer will instrument these calls.
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I);
  if (!II) {
    return false;
  }
  if (!llvm::is_contained(ValidIDs, II->getIntrinsicID())) {
    return false;
  }

  return UBSanInst::is(I);
}

bool isUbsanFallbackBlock(const llvm::BasicBlock &BB) {
  if (!isNoSanitize(*BB.getTerminator())) {
    return false;
  }
  // Has only one predecessor.
  if (BB.getSinglePredecessor() == nullptr) {
    return false;
  }

  return any_of(BB, isUbsanReportCall);
}

} // namespace __xsan
