#pragma once

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

#include <array>
#include <variant>
#include <vector>

namespace __xsan::desan {

inline constexpr char UserInstMDKind[] = "xsan.user";
inline constexpr std::array<const char *, 3> InterceptionTargetProcesses{
    "clang", "clang++", "opt"};
inline constexpr char InterceptionGroup[] = "desan";

struct DeleteEvent {
  llvm::Instruction *DeletedValue = nullptr;
  llvm::SmallVector<llvm::Value *, 4> Operands;
  // When BasicBlock ptr is used, the anchor is at the end of the block.
  // Otherwise, the anchor is before the Instruction ptr.
  std::variant<llvm::BasicBlock *, llvm::Instruction *> Anchor;
};

struct ReplaceEvent {
  llvm::Value *UserValue = nullptr;
  llvm::Value *ReplaceValue = nullptr;
};

void resetSession();
void beginRecording();
void endRecording();
bool isRecording();
std::vector<DeleteEvent> getDeleteEvents();
std::vector<ReplaceEvent> getReplaceEvents();

void recordDeleteEvent(llvm::Instruction *I);
void recordReplaceEvent(llvm::Value *Old, llvm::Value *New);
bool hasUserMark(const llvm::Instruction &I);

} // namespace __xsan::desan
