#pragma once

#include "livepatch/XSanDeSanRuntime.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace __xsan::desan {

struct AddRecord {
  constexpr static char MagicRecordBbName[] = "__xsan_desan_add_record_bb";

  enum class AddRecordKind : uint8_t {
    Pure,
    Split,
  };

  struct PureRecord {
    std::unique_ptr<llvm::BasicBlock> SanInsts;
  };

  struct SplitRecord {
    std::unique_ptr<llvm::BasicBlock> SanInstsToAppend;
    llvm::BasicBlock *LastBB;
  };

  StageKind Stage;
  AddRecordKind Kind;
  // The first user instruction after the added parts.
  llvm::Instruction *Anchor;
  std::variant<PureRecord, SplitRecord> Record;
};

struct ReplaceRecord {
  StageKind Stage = StageKind::None;
  llvm::Value *SanitizerValue = nullptr;
};

struct DeleteRecord {
  StageKind Stage = StageKind::None;
};

struct FunctionRecord {
  StageKind Stage = StageKind::None;
  llvm::Function *Func = nullptr;
};

struct AddArchive {
  // When Function ptr is used, the anchor is at the end of the function.
  // Otherwise, the anchor is before the BasicBlock ptr.
  using BBAnchor = void *;
  enum BBAnchorType {
    FunctionAnchor,
    BasicBlockAnchor,
  };

  std::vector<AddRecord> Records;
  // We don't use std::variant for the anchor type because llvm::DenseMap
  // doesn't support it as the key type. And write a custom DenseMapInfo is too
  // long.
  llvm::DenseMap<BBAnchor,
                 std::pair<BBAnchorType, std::vector<llvm::BasicBlock *>>>
      SanBBMap;
};

using DeleteArchive =
    llvm::DenseMap<llvm::Instruction *, llvm::SmallVector<DeleteRecord, 2>>;

using ReplaceArchive =
    llvm::DenseMap<llvm::Value *, llvm::SmallVector<ReplaceRecord, 2>>;

using FunctionArchive = llvm::SmallVector<FunctionRecord, 2>;

struct ReSanArchive {
  AddArchive AddRecords;
  DeleteArchive DeleteRecords;
  ReplaceArchive ReplaceRecords;
  FunctionArchive FunctionRecords;
};

struct ResidualSanitizerSummary {
  size_t LiveInstructionCount = 0;
  size_t SanitizerOnlyBlockCount = 0;
  size_t SanitizerOnlyBlockInstructionCount = 0;
  llvm::SmallVector<llvm::BasicBlock *, 8> SanitizerOnlyBlocks;
};

using UserBBsSet = llvm::SmallPtrSet<llvm::BasicBlock *, 8>;
using UserBBsMap = llvm::DenseMap<llvm::Function *, UserBBsSet>;

struct SanitizingContext {
  ReSanArchive Archive;
  UserBBsMap UserBBs;
};

// Utilities.
void requireOrDie(bool Condition, const llvm::Twine &Message);

void markUserIR(llvm::Module &M);
UserBBsMap collectUserBBs(llvm::Module &M);

bool isStageIR(StageKind Stage, const llvm::Instruction &I);
void setStageIR(StageKind Stage, llvm::Instruction &I);

bool replaceUserIRUses(llvm::Value &From, llvm::Value &To);
bool replaceNonStageIRUses(StageKind Stage, llvm::Value &From, llvm::Value &To);

// Main logic.
void runDeSan(llvm::Module &M, StageKind Stage, ReSanArchive &Archive,
              UserBBsMap &UserBBs);
void runReSan(llvm::Module &M, ReSanArchive &Archive);

// For debug and verification.
void dumpArchive(const ReSanArchive &Archive, llvm::raw_ostream &OS);
void validateArchive(const ReSanArchive &Archive, const UserBBsMap &UserBBs);

} // namespace __xsan::desan
