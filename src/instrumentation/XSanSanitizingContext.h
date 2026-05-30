#pragma once

#include "livepatch/record/SanitizingRecorder.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <memory>
#include <optional>
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
  static constexpr const char *UBSanName = "ubsan";

public:
  Sanitizer() = default;

  static Sanitizer Create(llvm::StringRef Name);
  static const Sanitizer &UBSan() {
    static Sanitizer UBSan = []() {
      Sanitizer San = Create(UBSanName);
      San.IsUBSan = true;
      return San;
    }();
    return UBSan;
  }

  llvm::StringRef name() const { return Name; }
  size_t order() const { return Order; }
  bool isUBSan() const { return IsUBSan; }
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
  bool IsUBSan = false;
  size_t Order = 0;
  mutable unsigned SanID = 0;
  mutable const llvm::LLVMContext *SanIDContext = nullptr;
};

/// Represents SanInsts
///  ([SanInst,...], UsrInstAnchor)
struct AddRecord {
  constexpr static char MagicRecordBbName[] = "__xsan_desan_add_record_bb";
  using SanBBsVec = llvm::SmallVector<llvm::BasicBlock *, 2>;

  enum class AddRecordKind : uint8_t {
    Pure,     // Add Seq inside a user BB
    Split,    // Add Seq split a user BB to source -> sanBB -> sink
    Redirect, // Add Seq redirect source -> sink to source -> sanBB -> sink
  };

  struct PureExtra {};

  struct SplitExtra {
    // Source User BB, unstable.
    // When replaying, the real source BB, i.e., BB(Anchor), might be another
    // SinkBB.
    llvm::BasicBlock *const SrcBB;
    // Sink San BB, stable.
    llvm::BasicBlock *const SinkBB;
  };

  struct RedirectExtra : SplitExtra {
    // not contains SourceBB, as it is unstable while replaying.
    llvm::SmallSetVector<llvm::BasicBlock *, 2> DetachedEdges;
    void removeEdgesOfSanBB2Sink(llvm::BasicBlock &SourceBB,
                                 llvm::BasicBlock &SanInsts);
  };

  AddRecord(AddRecord &&) noexcept = default;
  AddRecord &operator=(AddRecord &&) noexcept = delete;
  AddRecord(const AddRecord &) = delete;
  AddRecord &operator=(const AddRecord &) = delete;

  AddRecordKind Kind;
  // The first user instruction after the added parts.
  llvm::Instruction *const Anchor;
  // The related SanInsts
  std::unique_ptr<llvm::BasicBlock> SanInsts;
  // The related SanBBs.
  SanBBsVec SanBBs;
  std::variant<PureExtra, SplitExtra, RedirectExtra> ExtraRecord;

  // Construct a pure add record and move SrcBB[Fr:To) into it.
  static AddRecord extractFrom(llvm::BasicBlock &SrcBB,
                               llvm::BasicBlock::iterator Fr,
                               llvm::BasicBlock::iterator To);
  // Construct a split add record, move SrcBB[Fr:end) into it, then move
  // SinkBB[To:end) back to SrcBB.end().
  static AddRecord
  extractFrom(llvm::BasicBlock &SrcBB, llvm::BasicBlock::iterator Fr,
              llvm::BasicBlock &SinkBB, llvm::BasicBlock::iterator To,
              SanBBsVec &&SanBBs);
  // Construct a shared-sink split record, move SrcBB[Fr:end) into it,
  // and replace the source suffix with an unconditional branch to SinkBB.
  static AddRecord
  extractFrom(llvm::BasicBlock &SrcBB, llvm::BasicBlock::iterator Fr,
              llvm::BasicBlock &SinkBB,
              llvm::SmallSetVector<llvm::BasicBlock *, 2> &&DetachedEdges,
              SanBBsVec &&SanBBs);

};

/// Represents SanPhis inserted on UsrBB
///  ([SanPhi,...], UsrBB)
struct PHIAddRecord {
  /// The size of SanPhis should be small, vec is enough, not requiring linked
  /// list BasicBlock.
  llvm::SmallVector<llvm::PHINode *, 2> SanPhis;
  llvm::BasicBlock *const TargetUsrBB;

  static std::optional<PHIAddRecord> extractFrom(const Sanitizer &San,
                                                 llvm::BasicBlock &TargetUsrBB);
};

/// Represents SanBBs
///  ([SanBB,...], UsrBB)
struct BlockAddRecord {
  using SanBBVec = llvm::SmallVector<llvm::BasicBlock *, 2>;
  llvm::Function *Parent = nullptr;
  SanBBVec SanBBs;
  // Null means append to Parent->end().
  llvm::BasicBlock *InsertPt = nullptr;
  bool Replayed = false;

  void revert();
};

struct ReplaceRecord {
  llvm::Value *Replacer = nullptr;
  // If RealReplace = false, the replacement happens logically without any
  // effects. E.g., the replacement of `void` instruction.
  bool RealReplace = false;
};

struct SanitizingContext;

// Per-sanitizer archive of added IR. Records stores in-block sanitizer
// instruction sequences, while BBRecords stores detached sanitizer-only blocks
// in original order together with their reinsertion point.
struct AddArchive {
  AddArchive(SanitizingContext &Ctx) : Ctx(Ctx) {};
  AddArchive(AddArchive &&) noexcept = default;
  AddArchive(const AddArchive &) = delete;
  AddArchive &operator=(const AddArchive &) = delete;

  std::vector<AddRecord> Records;
  std::vector<BlockAddRecord> BBRecords;
  std::vector<PHIAddRecord> PhiRecords;
  SanitizingContext &Ctx;

  void addRecord(AddRecord &&Add);
  void tagAs(const Sanitizer &San);
};

class DeleteArchive : public llvm::SmallSetVector<llvm::Instruction *, 8> {
public:
  /// @return restore successfully or not.
  bool restoreEvent(DeleteEvent &);
};

using ReplaceArchive =
    llvm::DenseMap<llvm::Value *, llvm::SmallVector<ReplaceRecord, 2>>;

using FunctionArchive = llvm::SmallVector<llvm::Function *, 2>;

// All ReSan replay material for one sanitizer. The owning sanitizer identity is
// intentionally outside this struct (SanitizerArchive) so individual records do
// not duplicate a stage/sanitizer field.
struct ReSanArchive {
  ReSanArchive(SanitizingContext &Ctx) : AddRecords(Ctx) {}
  ReSanArchive(ReSanArchive &&) noexcept = default;
  ReSanArchive(const ReSanArchive &) = delete;
  ReSanArchive &operator=(const ReSanArchive &) = delete;

  AddArchive AddRecords;
  ReplaceArchive ReplaceRecords;
  DeleteArchive DeleteRecords;
  FunctionArchive FunctionRecords;

  void clear() {
    AddRecords.Records.clear();
    AddRecords.BBRecords.clear();
    AddRecords.PhiRecords.clear();
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
  SanitizerArchive(SanitizingContext &Context, const Sanitizer &San)
      : Context(Context), San(San), Archive{Context} {}
  SanitizerArchive(SanitizerArchive &&) noexcept = default;
  SanitizerArchive(const SanitizerArchive &) = delete;
  SanitizerArchive &operator=(const SanitizerArchive &) = delete;

  SanitizingContext &Context;
  Sanitizer San;
  ReSanArchive Archive;
};

struct SanitizingContext {
  static constexpr unsigned NoDumpIndex = ~0u;

  UserBBsMap UserBBs;
  bool UBSanDeSanCompleted = false;
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
