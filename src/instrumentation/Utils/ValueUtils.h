#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Value.h"

namespace __xsan {

inline bool isNoSanitize(const llvm::Instruction &I) {
  return I.hasMetadata(llvm::LLVMContext::MD_nosanitize);
}

bool shouldSkip(const llvm::Instruction &I);

const llvm::Value *extractAddrFromLoadStoreInst(const llvm::Instruction &I);

bool addrPointsToConstantDataAggressive(const llvm::Value *Addr);

/// `alloc` is handled in other place, this function is for other cases.
/// 1. `byval` pointer argument
/// 2. `noalias` call return value
/// 3. `alloc` stack object
/// Note that `noalias` argument should have been a function local, but because
/// it belongs to capture by return in the context of TSan, it is not included.
bool isUncapturedFuncLocal(const llvm::Value &Addr);

// Check whether the Instruction is on the top of the BB (ignore debug/phi
// instruction). If so, just return its parent BB address. Otherwise, split the
// BB on the location of this instruction, and return the new split BB address.
llvm::BlockAddress *getBlockAddressOfInstruction(llvm::Instruction &I,
                                                 llvm::DominatorTree *DT,
                                                 llvm::LoopInfo *LI,
                                                 llvm::MemorySSAUpdater *MSSAU);

/// Returns unique alloca or replaced alloca where the value comes from, or
/// nullptr. If OffsetZero is true check that V points to the begining of the
/// alloca.
llvm::Instruction *findAllocaForValue(llvm::Value *V, bool OffsetZero = false);

namespace {
inline constexpr llvm::StringLiteral
    kXsanLifetimeStartName("xsan.lifetime_start");
inline constexpr llvm::StringLiteral kXsanLifetimeEndName("xsan.lifetime_end");

} // namespace

inline bool isXsanLifetimeStart(const llvm::IntrinsicInst &I) {
  return I.getIntrinsicID() == llvm::Intrinsic::donothing &&
         I.getOperandBundle(kXsanLifetimeStartName).has_value();
}
inline bool isXsanLifetimeEnd(const llvm::IntrinsicInst &I) {
  return I.getIntrinsicID() == llvm::Intrinsic::donothing &&
         I.getOperandBundle(kXsanLifetimeEndName).has_value();
}
inline llvm::Value *getLifetimeStartOperand(const llvm::IntrinsicInst &I) {
  if (I.getIntrinsicID() == llvm::Intrinsic::lifetime_start)
    return I.getArgOperand(0);
  assert(isXsanLifetimeStart(I));
  auto Bundle = I.getOperandBundle(kXsanLifetimeStartName);
  assert(Bundle && Bundle->Inputs.size() == 1);
  return Bundle->Inputs.front().get();
}
void insertXSanLifetime(llvm::IntrinsicInst &II);

inline void
addNoInstrumentationAttr(llvm::ArrayRef<llvm::Function *> Functions) {
  for (auto *F : Functions)
    if (F) {
      F->addFnAttr(llvm::Attribute::NoSanitizeCoverage);
      F->addFnAttr(llvm::Attribute::DisableSanitizerInstrumentation);
    }
}
} // namespace __xsan
