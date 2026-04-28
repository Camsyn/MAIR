#pragma once

#include "livepatch/record/SanitizingRecorder.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace __xsan::desan {

// A sanitizer is identified by the user-facing plan name ("asan", "msan",
// ...). Creation order is the replay order used by ReSan. Metadata is derived
// from the name as `xsan.<name>`, so adding a sanitizer no longer requires a
// new hard-coded metadata kind.
class Sanitizer {
public:
  Sanitizer() = default;

  static Sanitizer Create(llvm::StringRef Name);

  llvm::StringRef name() const { return Name; }
  size_t order() const { return Order; }
  bool isUBSan() const { return Name == "ubsan"; }
  bool sameAs(const Sanitizer &Other) const {
    return Order == Other.Order && Name == Other.Name;
  }
  bool isLaterThan(const Sanitizer &Other) const { return Order > Other.Order; }

  bool is(const llvm::Instruction &I) const;
  void tag(llvm::Instruction &I) const;

private:
  explicit Sanitizer(llvm::StringRef Name);
  unsigned getSanID(llvm::LLVMContext &Ctx) const;

  std::string Name;
  size_t Order = 0;
  mutable unsigned SanID = 0;
  mutable const llvm::LLVMContext *SanIDContext = nullptr;
};

// Describes sanitizer IR that was removed by DeSan and must be reinserted by
// ReSan. An add can be either a pure instruction sequence before a user
// instruction, or a split edge where sanitizer blocks were interleaved with
// user blocks.
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

  AddRecord() = default;
  AddRecord(AddRecord &&) noexcept = default;
  AddRecord &operator=(AddRecord &&) noexcept = default;
  AddRecord(const AddRecord &) = delete;
  AddRecord &operator=(const AddRecord &) = delete;

  AddRecordKind Kind;
  // The first user instruction after the added parts.
  llvm::Instruction *Anchor;
  std::variant<PureRecord, SplitRecord> Record;
};

struct ReplaceRecord {
  llvm::Value *SanitizerValue = nullptr;
};

struct DeleteRecord {};

// Per-sanitizer archive of added IR. SanBBMap holds detached sanitizer-only
// blocks keyed by their original insertion anchor. DenseMap cannot directly key
// on a Function/BasicBlock variant, so the anchor is stored as a typed pointer
// pair.
struct AddArchive {
  AddArchive() = default;
  AddArchive(AddArchive &&) noexcept = default;
  AddArchive &operator=(AddArchive &&) noexcept = default;
  AddArchive(const AddArchive &) = delete;
  AddArchive &operator=(const AddArchive &) = delete;

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

using FunctionArchive = llvm::SmallVector<llvm::Function *, 2>;

// All ReSan replay material for one sanitizer. The owning sanitizer identity is
// intentionally outside this struct (SanitizerArchive) so individual records do
// not duplicate a stage/sanitizer field.
struct ReSanArchive {
  ReSanArchive() = default;
  ReSanArchive(ReSanArchive &&) noexcept = default;
  ReSanArchive &operator=(ReSanArchive &&) noexcept = default;
  ReSanArchive(const ReSanArchive &) = delete;
  ReSanArchive &operator=(const ReSanArchive &) = delete;

  AddArchive AddRecords;
  ReplaceArchive ReplaceRecords;
  DeleteArchive DeleteRecords;
  FunctionArchive FunctionRecords;

  void clear() {
    AddRecords.Records.clear();
    AddRecords.SanBBMap.clear();
    DeleteRecords.clear();
    ReplaceRecords.clear();
    FunctionRecords.clear();
  }
};

struct ResidualSanitizerSummary {
  size_t LiveInstructionCount = 0;
  size_t SanitizerOnlyBlockCount = 0;
  size_t SanitizerOnlyBlockInstructionCount = 0;
  llvm::SmallVector<llvm::BasicBlock *, 8> SanitizerOnlyBlocks;
};

using UserBBsSet = llvm::SmallPtrSet<llvm::BasicBlock *, 8>;
using UserBBsMap = llvm::DenseMap<llvm::Function *, UserBBsSet>;

struct SanitizerArchive {
  SanitizerArchive(const Sanitizer &San) : San(San) {}
  SanitizerArchive(SanitizerArchive &&) noexcept = default;
  SanitizerArchive(const SanitizerArchive &) = delete;
  SanitizerArchive &operator=(const SanitizerArchive &) = delete;

  Sanitizer San;
  ReSanArchive Archive;
};

struct SanitizingContext {
  static constexpr unsigned NoDumpIndex = ~0u;

  UserBBsMap UserBBs;
  // Runtime archive order. Each entry is appended by DeSanPass::run and then
  // replayed by ReSan in the same dynamic order.
  llvm::SmallVector<SanitizerArchive, 4> SanitizerArchives;

  void clearArchives();
  SanitizerArchive &addArchive(const Sanitizer &San);

  void resetDumpSequence() { NextDumpIndex = 0; }
  unsigned reserveDumpIndex();
  void dumpIR(llvm::Module &M, unsigned Index, const llvm::Twine &Name) const;
  unsigned dumpNextIR(llvm::Module &M, const llvm::Twine &Name);
  void dumpNestedIR(llvm::Module &M, unsigned ParentIndex,
                    const llvm::Twine &Name) const;

private:
  unsigned NextDumpIndex = 0;
};

// Utilities.
void requireOrDie(bool Condition, const llvm::Twine &Message);

void markUserIR(llvm::Module &M);
UserBBsMap collectUserBBs(llvm::Module &M);

bool replaceUserIRUses(llvm::Value &From, llvm::Value &To);
bool replaceNonSanitizerUses(const Sanitizer &San, llvm::Value &From,
                             llvm::Value &To);

// Main logic.
void runDeSan(llvm::Module &M, SanitizerArchive &Archive, UserBBsMap &UserBBs);
void runReSan(llvm::Module &M, SanitizingContext &Context,
              unsigned DumpIndex = SanitizingContext::NoDumpIndex);

// For debug and verification.
void dumpArchive(const Sanitizer &San, const ReSanArchive &Archive,
                 llvm::raw_ostream &OS);
void validateArchive(const Sanitizer &San, const ReSanArchive &Archive,
                     const UserBBsMap &UserBBs);

} // namespace __xsan::desan
