#include "../../../include/xsan_llvm_compat.h"
#include "../../include/xsan_desan_abi.h"
#include "utils/PatchHelper.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

namespace __xsan::desan {
namespace {

struct StageRecorder {
  std::mutex Mutex;
  bool Active = false;
  XSanDeSanStageKind Stage = XSAN_DESAN_STAGE_NONE;
  uint64_t Sequence = 0;
  std::vector<XSanDeSanDeleteEvent> DeleteEvents;
  std::vector<XSanDeSanDeleteOperand> DeleteOperands;
  llvm::DenseMap<const llvm::Instruction *,
                 llvm::SmallVector<XSanDeSanDeleteEvent *, 1>>
      DeleteAnchorMap;
  std::vector<XSanDeSanReplaceEvent> ReplaceEvents;
  llvm::DenseMap<const llvm::Value *,
                 llvm::SmallVector<XSanDeSanReplaceEvent *, 1>>
      ReplaceAnchorMap;
  llvm::DenseMap<const llvm::Instruction *, llvm::SmallVector<llvm::Value *, 4>>
      CachedOperandMap;
  // Not recorded for now.
  std::vector<XSanDeSanDeleteBlock> DeleteBlocks;
};

StageRecorder &getRecorder() {
  static StageRecorder Recorder;
  return Recorder;
}

void resetStatsLocked(StageRecorder &Recorder) {
  Recorder.DeleteEvents.clear();
  Recorder.DeleteOperands.clear();
  Recorder.DeleteAnchorMap.clear();
  Recorder.ReplaceEvents.clear();
  Recorder.ReplaceAnchorMap.clear();
  Recorder.CachedOperandMap.clear();
  Recorder.DeleteBlocks.clear();
}

bool shouldRecordLocked(const StageRecorder &Recorder) {
  return Recorder.Active;
}

bool hasUserMark(const llvm::Instruction &I) {
  return I.hasMetadata(XSanDeSanUserInstMDKind);
}

XSanDeSanDeleteEvent buildDeleteEvent(const llvm::Instruction &I) {
  XSanDeSanDeleteEvent Event;
  Event.deleted_value = reinterpret_cast<uint64_t>(&I);
  if (&I == &I.getParent()->back()) {
    Event.anchor_kind = XSAN_DESAN_DELETE_ANCHOR_BB;
    Event.anchor_ptr = reinterpret_cast<uint64_t>(I.getParent());
  } else {
    Event.anchor_kind = XSAN_DESAN_DELETE_ANCHOR_INST;
    Event.anchor_ptr = reinterpret_cast<uint64_t>(I.getNextNode());
  }
  return Event;
}

XSanDeSanReplaceEvent buildReplaceEvent(const llvm::Value &Old,
                                        const llvm::Value &New) {
  XSanDeSanReplaceEvent Event;
  Event.user_value = reinterpret_cast<uint64_t>(&Old);
  Event.replace_value = reinterpret_cast<uint64_t>(&New);
  return Event;
}

} // namespace

void recordDeleteEvent(llvm::Instruction *I) {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  if (!shouldRecordLocked(Recorder) || !I || !I->getParent())
    return;

  if (!hasUserMark(*I)) {
    // This non-user instruction was already an anchor, update all events
    // anchored to it. We don't do update for user instructions because
    // their order should be preserved and we can restore them unlike deleted
    // non-user instructions.
    if (auto It = Recorder.DeleteAnchorMap.find(I);
        It != Recorder.DeleteAnchorMap.end()) {
      auto UpdatedEvent = buildDeleteEvent(*I);

      for (auto *Event : It->second) {
        Event->anchor_kind = UpdatedEvent.anchor_kind;
        Event->anchor_ptr = UpdatedEvent.anchor_ptr;
      }

      // New anchor still a non-user instruction, update its mapped events.
      if (auto *NewAnchorInst =
              reinterpret_cast<llvm::Instruction *>(UpdatedEvent.anchor_ptr);
          UpdatedEvent.anchor_kind == XSAN_DESAN_DELETE_ANCHOR_INST &&
          !hasUserMark(*NewAnchorInst)) {
        if (auto NewIt = Recorder.DeleteAnchorMap.find(NewAnchorInst);
            NewIt != Recorder.DeleteAnchorMap.end()) {
          NewIt->second.resize(NewIt->second.size() + It->second.size());
          std::copy(It->second.begin(), It->second.end(),
                    NewIt->second.end() - It->second.size());
          Recorder.DeleteAnchorMap.erase(It);
        } else {
          Recorder.DeleteAnchorMap[NewAnchorInst] = std::move(It->second);
        }
      } else {
        Recorder.DeleteAnchorMap.erase(It);
      }
    }
  } else {
    auto Event = buildDeleteEvent(*I);
    Recorder.DeleteEvents.push_back(Event);

    // We need to also eliminate operands in case the operands can find the
    // deleted instruction through their user list.
    llvm::SmallVector<llvm::Value *, 4> *CachedOperands = nullptr;
    if (auto It = Recorder.CachedOperandMap.find(I);
        It != Recorder.CachedOperandMap.end())
      CachedOperands = &It->second;
    for (size_t i = 0; i < I->getNumOperands(); ++i) {
      llvm::Value *Operand =
          CachedOperands ? (*CachedOperands)[i] : I->getOperand(i);
      Recorder.DeleteOperands.push_back({reinterpret_cast<uint64_t>(Operand)});
      I->setOperand(i, llvm::UndefValue::get(Operand->getType()));
    }
    Recorder.DeleteOperands.push_back(XSanDeSanDeleteOperandNull);

    // If new anchor is a non-user instruction, update its mapped events.
    if (auto *AnchorInst =
            reinterpret_cast<llvm::Instruction *>(Event.anchor_ptr);
        Event.anchor_kind == XSAN_DESAN_DELETE_ANCHOR_INST &&
        !hasUserMark(*AnchorInst)) {
      Recorder.DeleteAnchorMap[AnchorInst].push_back(
          &Recorder.DeleteEvents.back());
    }
  }
}

void recordReplaceEvent(llvm::Value *Old, llvm::Value *New) {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  if (!shouldRecordLocked(Recorder) || !Old || !New)
    return;

  // if not a argument or user-marked instruction, treat as non-user value
  if (!llvm::isa<llvm::Argument>(Old) &&
      !(llvm::isa<llvm::Instruction>(Old) &&
        hasUserMark(*llvm::cast<llvm::Instruction>(Old)))) {
    if (auto It = Recorder.ReplaceAnchorMap.find(Old);
        It != Recorder.ReplaceAnchorMap.end()) {
      // This non-user value was already an anchor, update all events anchored
      // to it
      auto UpdatedEvent = buildReplaceEvent(*Old, *New);

      for (auto *Event : It->second)
        Event->replace_value = UpdatedEvent.replace_value;

      if (auto NewIt = Recorder.ReplaceAnchorMap.find(New);
          NewIt != Recorder.ReplaceAnchorMap.end()) {
        NewIt->second.resize(NewIt->second.size() + It->second.size());
        std::copy(It->second.begin(), It->second.end(),
                  NewIt->second.end() - It->second.size());
        Recorder.ReplaceAnchorMap.erase(It);
      } else {
        Recorder.ReplaceAnchorMap[New] = std::move(It->second);
      }
    }
  } else {
    Recorder.ReplaceEvents.push_back(buildReplaceEvent(*Old, *New));
    Recorder.ReplaceAnchorMap[New].push_back(&Recorder.ReplaceEvents.back());

    // If the old value is used as operand of any user instruction, we also need
    // to cache the operand list of that user instruction so that we can collect
    // correct operands when the user instruction is deleted.
    for (llvm::User *U : Old->users()) {
      if (auto *UserInst = llvm::dyn_cast<llvm::Instruction>(U);
          UserInst && hasUserMark(*UserInst)) {
        if (auto [It, Inserted] =
                Recorder.CachedOperandMap.try_emplace(UserInst);
            Inserted) {
          It->second.append(UserInst->op_begin(), UserInst->op_end());
        }
      }
    }
  }
}

void recordDeleteBlock(llvm::BasicBlock *BB) {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  if (!shouldRecordLocked(Recorder) || !BB || !BB->getParent())
    return;

  Recorder.DeleteBlocks.push_back({reinterpret_cast<uint64_t>(BB->getParent()),
                                   reinterpret_cast<uint64_t>(BB)});
}

} // namespace __xsan::desan

extern "C" void __xsan_desan_reset_session(void) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  Recorder.Active = false;
  Recorder.Stage = XSAN_DESAN_STAGE_NONE;
  Recorder.Sequence = 0;
}

extern "C" void __xsan_desan_begin_stage(uint32_t StageKind) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  Recorder.Active = true;
  Recorder.Stage = static_cast<XSanDeSanStageKind>(StageKind);
  ++Recorder.Sequence;
  __xsan::desan::resetStatsLocked(Recorder);
}

extern "C" void __xsan_desan_end_stage() {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  Recorder.Active = false;
  Recorder.Stage = XSAN_DESAN_STAGE_NONE;
}

extern "C" int __xsan_desan_is_stage_active(void) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.Active ? 1 : 0;
}

extern "C" uint64_t __xsan_desan_get_delete_event_count(void) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.DeleteEvents.size();
}

extern "C" uint64_t
__xsan_desan_copy_delete_events(XSanDeSanDeleteEvent *OutEvents,
                                uint64_t Capacity) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  const uint64_t Count = Recorder.DeleteEvents.size();
  if (!OutEvents || Capacity == 0)
    return Count;

  const uint64_t ToCopy = Count < Capacity ? Count : Capacity;
  std::memcpy(OutEvents, Recorder.DeleteEvents.data(),
              sizeof(XSanDeSanDeleteEvent) * ToCopy);
  return Count;
}

extern "C" uint64_t __xsan_desan_get_delete_operands_count(void) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.DeleteOperands.size();
}

extern "C" uint64_t
__xsan_desan_copy_delete_operands(XSanDeSanDeleteOperand *OutOperands,
                                  uint64_t Capacity) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  const uint64_t Count = Recorder.DeleteOperands.size();
  if (!OutOperands || Capacity == 0)
    return Count;

  const uint64_t ToCopy = Count < Capacity ? Count : Capacity;
  std::memcpy(OutOperands, Recorder.DeleteOperands.data(),
              sizeof(XSanDeSanDeleteOperand) * ToCopy);
  return Count;
}

extern "C" uint64_t __xsan_desan_get_replace_event_count(void) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.ReplaceEvents.size();
}

extern "C" uint64_t
__xsan_desan_copy_replace_events(XSanDeSanReplaceEvent *OutEvents,
                                 uint64_t Capacity) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  const uint64_t Count = Recorder.ReplaceEvents.size();
  if (!OutEvents || Capacity == 0)
    return Count;

  const uint64_t ToCopy = Count < Capacity ? Count : Capacity;
  std::memcpy(OutEvents, Recorder.ReplaceEvents.data(),
              sizeof(XSanDeSanReplaceEvent) * ToCopy);
  return Count;
}

extern "C" uint64_t __xsan_desan_get_delete_block_count(void) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.DeleteBlocks.size();
}

extern "C" uint64_t
__xsan_desan_copy_delete_blocks(XSanDeSanDeleteBlock *OutBlocks,
                                uint64_t Capacity) {
  auto &Recorder = __xsan::desan::getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  const uint64_t Count = Recorder.DeleteBlocks.size();
  if (!OutBlocks || Capacity == 0)
    return Count;

  const uint64_t ToCopy = Count < Capacity ? Count : Capacity;
  std::memcpy(OutBlocks, Recorder.DeleteBlocks.data(),
              sizeof(XSanDeSanDeleteBlock) * ToCopy);
  return Count;
}

namespace llvm {

static __xsan::XsanInterceptor
    RemoveFromParentInterceptor(&llvm::Instruction::removeFromParent,
                                {"clang", "clang++"});

void Instruction::removeFromParent() {
  __xsan::desan::recordDeleteEvent(this);
  RemoveFromParentInterceptor(this);
}

static __xsan::XsanInterceptor
    EraseFromParentInterceptor(&llvm::Instruction::eraseFromParent,
                               {"clang", "clang++"});

__xsan::compat::InstructionEraseIterator Instruction::eraseFromParent() {
  __xsan::desan::recordDeleteEvent(this);
  if (__xsan_desan_is_stage_active() && __xsan::desan::hasUserMark(*this)) {
    auto Next = std::next(getIterator());
    RemoveFromParentInterceptor(this);
    return Next;
  }
  return EraseFromParentInterceptor(this);
}

static __xsan::XsanInterceptor
    ReplaceAllUsesWithInterceptor(&llvm::Value::replaceAllUsesWith,
                                  {"clang", "clang++"});

void Value::replaceAllUsesWith(Value *V) {
  // RAUW can recurse when a Constant user needs to rebuild itself.
  // For example, replacing an operand of a Global may trigger
  // Constant::handleOperandChange(), which then performs another RAUW on the
  // rebuilt Constant/Global. We only record the outermost RAUW semantic event;
  // recursive RAUW calls reuse the same interceptor critical section.
  if (!ReplaceAllUsesWithInterceptor.isRecursive())
    __xsan::desan::recordReplaceEvent(this, V);
  ReplaceAllUsesWithInterceptor(this, V);
}

using ReplaceInstWithInstFn = void (*)(llvm::Instruction *,
                                       llvm::Instruction *);

static __xsan::XsanInterceptor<ReplaceInstWithInstFn>
    ReplaceInstWithInstInterceptor(
        static_cast<ReplaceInstWithInstFn>(&llvm::ReplaceInstWithInst),
        {"clang", "clang++"});

void ReplaceInstWithInst(Instruction *From, Instruction *To) {
  if (__xsan_desan_is_stage_active() && From && To) {
    if (__xsan::desan::hasUserMark(*From)) {
      if (!To->getDebugLoc())
        To->setDebugLoc(From->getDebugLoc());
      __xsan::compat::insertInstructionBefore(To, From);
      From->replaceAllUsesWith(To);
      __xsan::desan::recordDeleteEvent(From);
      RemoveFromParentInterceptor(From);
      return;
    }
  }
  ReplaceInstWithInstInterceptor(From, To);
}

// static __xsan::XsanInterceptor
//     BasicBlockRemoveFromParentInterceptor(&llvm::BasicBlock::removeFromParent,
//                                           {"clang", "clang++"});

// void BasicBlock::removeFromParent() {
//   __xsan::desan::recordDeleteBlock(this);
//   BasicBlockRemoveFromParentInterceptor(this);
// }

// static __xsan::XsanInterceptor
//     BasicBlockEraseFromParentInterceptor(&llvm::BasicBlock::eraseFromParent,
//                                          {"clang", "clang++"});

// SymbolTableList<BasicBlock>::iterator BasicBlock::eraseFromParent() {
//   __xsan::desan::recordDeleteBlock(this);
//   if (__xsan_desan_is_stage_active() && !getParent()->isDeclaration()) {
//     auto Next = std::next(getIterator());
//     BasicBlockRemoveFromParentInterceptor(this);
//     return Next;
//   }
//   return BasicBlockEraseFromParentInterceptor(this);
// }

} // namespace llvm
