#pragma once

#include <cstdint>
#include <utility>

#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Intrinsics.h"

namespace __xsan {
namespace compat {

// String starts_with compatibility
inline bool startsWith(llvm::StringRef Str, llvm::StringRef Prefix) {
#if LLVM_VERSION_MAJOR >= 16
  return Str.starts_with(Prefix);
#else
  return Str.startswith(Prefix);
#endif
}

// String ends_with compatibility
inline bool endsWith(llvm::StringRef Str, llvm::StringRef Suffix) {
#if LLVM_VERSION_MAJOR >= 16
  return Str.ends_with(Suffix);
#else
  return Str.endswith(Suffix);
#endif
}

// Get next non-debug instruction
inline llvm::Instruction *getNextNonDebugInstruction(llvm::Instruction *I) {
#if LLVM_VERSION_MAJOR >= 22
  // dbg has not been an instruction, see
  // https://llvm.org/docs/RemoveDIsDebugInfo.html
  return I->getNextNode();
#else
  return I->getNextNonDebugInstruction();
#endif
}

// Get next non-debug instruction (const version)
inline const llvm::Instruction *getNextNonDebugInstruction(
    const llvm::Instruction *I) {
  return getNextNonDebugInstruction(const_cast<llvm::Instruction *>(I));
}

// Get first non-PHI or debug or lifetime instruction
inline const llvm::Instruction *getFirstNonPHIOrDbgOrLifetime(
    const llvm::BasicBlock &BB) {
#if LLVM_VERSION_MAJOR >= 22
  auto It = BB.getFirstNonPHIOrDbgOrLifetime();
  return It != BB.end() ? &*It : nullptr;
#else
  return BB.getFirstNonPHIOrDbgOrLifetime();
#endif
}

// Count trailing zeros
inline unsigned countr_zero(uint64_t Val) {
#if LLVM_VERSION_MAJOR >= 22
  return llvm::countr_zero(Val);
#else
  return llvm::countTrailingZeros(Val);
#endif
}

inline llvm::Instruction *getFirstNonPHIIt(llvm::BasicBlock *BB) {
#if LLVM_VERSION_MAJOR >= 22
  return &*BB->getFirstNonPHIIt();
#else
  return BB->getFirstNonPHI();
#endif
}

using InstructionEraseIterator =
    decltype(std::declval<llvm::Instruction &>().eraseFromParent());

inline void insertInstructionBefore(llvm::Instruction *I,
                                    llvm::Instruction *InsertPos) {
#if LLVM_VERSION_MAJOR >= 22
  I->insertBefore(InsertPos->getIterator());
#else
  I->insertBefore(InsertPos);
#endif
}

inline void appendInstruction(llvm::BasicBlock *BB, llvm::Instruction *I) {
#if LLVM_VERSION_MAJOR >= 23
  I->insertInto(BB, BB->end());
#else
  BB->getInstList().push_back(I);
#endif
}

inline void spliceInstructions(llvm::BasicBlock *ToBB,
                               llvm::BasicBlock::iterator ToIt,
                               llvm::BasicBlock *FromBB) {
#if LLVM_VERSION_MAJOR >= 23
  ToBB->splice(ToIt, FromBB);
#else
  ToBB->getInstList().splice(ToIt, FromBB->getInstList());
#endif
}

inline void spliceInstructions(llvm::BasicBlock *ToBB,
                               llvm::BasicBlock::iterator ToIt,
                               llvm::BasicBlock *FromBB,
                               llvm::BasicBlock::iterator FromBegin,
                               llvm::BasicBlock::iterator FromEnd) {
#if LLVM_VERSION_MAJOR >= 23
  ToBB->splice(ToIt, FromBB, FromBegin, FromEnd);
#else
  ToBB->getInstList().splice(ToIt, FromBB->getInstList(), FromBegin, FromEnd);
#endif
}

inline llvm::Function *getOrInsertDeclaration(
    llvm::Module *M, llvm::Intrinsic::ID id,
    llvm::ArrayRef<llvm::Type *> Tys = {}) {
#if LLVM_VERSION_MAJOR >= 23
  return llvm::Intrinsic::getOrInsertDeclaration(M, id, Tys);
#else
  return llvm::Intrinsic::getDeclaration(M, id, Tys);
#endif
}

}  // namespace compat
}  // namespace __xsan
