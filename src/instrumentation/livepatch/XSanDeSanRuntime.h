#pragma once

#include "livepatch/xsan_desan_abi.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>

#include <cstdint>
#include <variant>
#include <vector>

namespace __xsan::desan {

enum class StageKind : uint32_t {
  None = XSAN_DESAN_STAGE_NONE,
  UBSan = XSAN_DESAN_STAGE_UBSAN,
  ASan = XSAN_DESAN_STAGE_ASAN,
  TSan = XSAN_DESAN_STAGE_TSAN,
  MSan = XSAN_DESAN_STAGE_MSAN,
  ReSan = XSAN_DESAN_STAGE_RESAN,
};

struct DeleteEvent {
  llvm::Instruction *DeletedValue;
  llvm::SmallVector<llvm::Value *, 4> Operands;
  // When BasicBlock ptr is used, the anchor is at the end of the block.
  // Otherwise, the anchor is before the Instruction ptr.
  std::variant<llvm::BasicBlock *, llvm::Instruction *> Anchor;
};

struct ReplaceEvent {
  llvm::Value *UserValue;
  llvm::Value *ReplaceValue;
};

struct DeleteBlock {
  llvm::Function *Func;
  llvm::BasicBlock *Block;
};

bool isRuntimeAvailable();
void resetSession();
void beginStage(StageKind Stage);
void endStage();
const char *getStageName(StageKind Stage);
std::vector<DeleteEvent> getDeleteEvents();
std::vector<ReplaceEvent> getReplaceEvents();
std::vector<DeleteBlock> getDeleteBlocks();

} // namespace __xsan::desan
