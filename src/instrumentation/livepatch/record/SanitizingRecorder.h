#pragma once

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

#include <array>
#include <vector>

namespace __xsan::desan {

template <typename EventT, typename AnchorT> class AnchoredEventStore;

inline constexpr char UserInstMDKind[] = "xsan.user";
inline constexpr std::array<const char *, 3> InterceptionTargetProcesses{
    "clang", "clang++", "opt"};
inline constexpr char InterceptionGroup[] = "desan";

struct DeleteEvent {
  friend class AnchoredEventStore<DeleteEvent, const llvm::Instruction *>;

  llvm::Instruction *const Inst2Remove = nullptr;
  llvm::BasicBlock *const Block = nullptr;
  llvm::Instruction *Anchor = nullptr;
  const llvm::SmallVector<llvm::Value *, 4> Operands;

private:
  void update(const llvm::Instruction *OldAnchor,
              const llvm::Instruction *NewAnchor) {
    // If OldAnchor != Anchor, it means that the Anchor points to other
    // DeleteEvent
    if (OldAnchor != Anchor) {
      assert((!Anchor || !Anchor->getParent()) &&
             "Current anchor should have been deleted");
      return;
    }
    Anchor = const_cast<llvm::Instruction *>(NewAnchor);
  }
};

struct ReplaceEvent {
  friend class AnchoredEventStore<ReplaceEvent, const llvm::Value *>;

  llvm::Value *UserReplacee = nullptr;
  llvm::Value *SanReplacer = nullptr;

private:
  void update(const llvm::Value *OldReplacer, const llvm::Value *Replacer) {
    (void)OldReplacer;
    SanReplacer = const_cast<llvm::Value *>(Replacer);
  }
};

void resetSession();
void beginRecording();
void endRecording();
bool isRecording();
const std::vector<DeleteEvent> &getDeleteEvents();
const std::vector<ReplaceEvent> &getReplaceEvents();

void recordDeleteEvent(llvm::Instruction *I);
void recordReplaceEvent(llvm::Value *Old, llvm::Value *New);

} // namespace __xsan::desan
