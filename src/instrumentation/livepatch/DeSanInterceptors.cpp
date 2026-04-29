#include "PatchCore.h"
#include "livepatch/XSanSanitizingRecorder.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include <iterator>
#include <mutex>
#include <vector>

namespace __xsan::desan {
namespace {

template <typename EventT, typename AnchorT> class AnchoredEventStore {
public:
  using IndexList = llvm::SmallVector<size_t, 1>;

  size_t append(EventT Event) {
    Events.push_back(std::move(Event));
    return Events.size() - 1;
  }

  void track(AnchorT Anchor, size_t EventIndex) {
    if (Anchor)
      AnchorMap[Anchor].push_back(EventIndex);
  }

  template <typename UpdateEventFn>
  void updateTracked(AnchorT OldAnchor, AnchorT NewAnchor,
                     UpdateEventFn UpdateEvent) {
    auto It = AnchorMap.find(OldAnchor);
    if (It == AnchorMap.end())
      return;

    for (size_t EventIndex : It->second)
      UpdateEvent(Events[EventIndex]);

    if (!NewAnchor) {
      AnchorMap.erase(It);
      return;
    }

    if (NewAnchor == OldAnchor)
      return;

    IndexList EventIndices = std::move(It->second);
    AnchorMap.erase(It);
    auto &NewIndices = AnchorMap[NewAnchor];
    NewIndices.append(EventIndices.begin(), EventIndices.end());
  }

  std::vector<EventT> copyEvents() const {
    return {Events.begin(), Events.end()};
  }

  void clear() {
    Events.clear();
    AnchorMap.clear();
  }

private:
  std::vector<EventT> Events;
  llvm::DenseMap<AnchorT, IndexList> AnchorMap;
};

struct SanitizingRecorder {
  std::mutex Mutex;
  bool Active = false;
  AnchoredEventStore<DeleteEvent, const llvm::Instruction *> DeleteEvents;
  AnchoredEventStore<ReplaceEvent, const llvm::Value *> ReplaceEvents;
  llvm::DenseMap<const llvm::Instruction *, llvm::SmallVector<llvm::Value *, 4>>
      CachedOperandMap;
};

SanitizingRecorder &getRecorder() {
  static SanitizingRecorder Recorder;
  return Recorder;
}

void resetStatsLocked(SanitizingRecorder &Recorder) {
  Recorder.DeleteEvents.clear();
  Recorder.ReplaceEvents.clear();
  Recorder.CachedOperandMap.clear();
}

bool shouldRecordLocked(const SanitizingRecorder &Recorder) {
  return Recorder.Active;
}

DeleteEvent buildDeleteEvent(const llvm::Instruction &I) {
  DeleteEvent Event;
  Event.DeletedValue = const_cast<llvm::Instruction *>(&I);
  if (&I == &I.getParent()->back())
    Event.Anchor = const_cast<llvm::BasicBlock *>(I.getParent());
  else
    Event.Anchor = const_cast<llvm::Instruction *>(I.getNextNode());
  return Event;
}

ReplaceEvent buildReplaceEvent(const llvm::Value &Old, const llvm::Value &New) {
  return {const_cast<llvm::Value *>(&Old), const_cast<llvm::Value *>(&New)};
}

llvm::Instruction *getInstructionAnchor(const DeleteEvent &Event) {
  if (auto *Anchor = std::get_if<llvm::Instruction *>(&Event.Anchor))
    return *Anchor;
  return nullptr;
}

const llvm::Instruction *getTrackableDeleteAnchor(const DeleteEvent &Event) {
  llvm::Instruction *Anchor = getInstructionAnchor(Event);
  if (Anchor && !hasUserMark(*Anchor))
    return Anchor;
  return nullptr;
}

bool shouldRecordReplaceSource(const llvm::Value &Old) {
  if (llvm::isa<llvm::Argument>(Old))
    return true;
  if (auto *I = llvm::dyn_cast<llvm::Instruction>(&Old))
    return hasUserMark(*I);
  return false;
}

void saveAndUndefineOperands(SanitizingRecorder &Recorder, llvm::Instruction &I,
                             DeleteEvent &Event) {
  llvm::SmallVector<llvm::Value *, 4> *CachedOperands = nullptr;
  if (auto It = Recorder.CachedOperandMap.find(&I);
      It != Recorder.CachedOperandMap.end())
    CachedOperands = &It->second;

  for (size_t IOp = 0; IOp < I.getNumOperands(); ++IOp) {
    llvm::Value *Operand =
        CachedOperands ? (*CachedOperands)[IOp] : I.getOperand(IOp);
    Event.Operands.push_back(Operand);
    I.setOperand(IOp, llvm::PoisonValue::get(Operand->getType()));
  }
}

void cacheUserOperandsUsing(SanitizingRecorder &Recorder, llvm::Value &Old) {
  for (llvm::User *U : Old.users()) {
    auto *UserInst = llvm::dyn_cast<llvm::Instruction>(U);
    if (!UserInst || !hasUserMark(*UserInst))
      continue;

    auto [It, Inserted] = Recorder.CachedOperandMap.try_emplace(UserInst);
    if (Inserted)
      It->second.append(UserInst->op_begin(), UserInst->op_end());
  }
}

} // namespace

bool hasUserMark(const llvm::Instruction &I) {
  return I.hasMetadata(UserInstMDKind);
}

void resetSession() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  Recorder.Active = false;
  resetStatsLocked(Recorder);
}

void beginRecording() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  Recorder.Active = true;
  resetStatsLocked(Recorder);
}

void endRecording() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  Recorder.Active = false;
}

bool isRecording() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.Active;
}

std::vector<DeleteEvent> getDeleteEvents() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.DeleteEvents.copyEvents();
}

std::vector<ReplaceEvent> getReplaceEvents() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.ReplaceEvents.copyEvents();
}

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
    auto UpdatedEvent = buildDeleteEvent(*I);
    Recorder.DeleteEvents.updateTracked(I,
                                        getTrackableDeleteAnchor(UpdatedEvent),
                                        [&UpdatedEvent](DeleteEvent &Event) {
                                          Event.Anchor = UpdatedEvent.Anchor;
                                        });
  } else {
    auto Event = buildDeleteEvent(*I);
    // We need to also eliminate operands in case the operands can find the
    // deleted instruction through their user list.
    saveAndUndefineOperands(Recorder, *I, Event);

    const llvm::Instruction *AnchorInst = getTrackableDeleteAnchor(Event);
    const size_t SavedEventIndex =
        Recorder.DeleteEvents.append(std::move(Event));

    // If new anchor is a non-user instruction, update its mapped events.
    Recorder.DeleteEvents.track(AnchorInst, SavedEventIndex);
  }
}

void recordReplaceEvent(llvm::Value *Old, llvm::Value *New) {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  if (!shouldRecordLocked(Recorder) || !Old || !New)
    return;

  if (!shouldRecordReplaceSource(*Old)) {
    auto UpdatedEvent = buildReplaceEvent(*Old, *New);
    Recorder.ReplaceEvents.updateTracked(
        Old, New, [&UpdatedEvent](ReplaceEvent &Event) {
          Event.ReplaceValue = UpdatedEvent.ReplaceValue;
        });
  } else {
    size_t EventIndex =
        Recorder.ReplaceEvents.append(buildReplaceEvent(*Old, *New));
    Recorder.ReplaceEvents.track(New, EventIndex);

    // If the old value is used as operand of any user instruction, we also need
    // to cache the operand list of that user instruction so that we can collect
    // correct operands when the user instruction is deleted.
    cacheUserOperandsUsing(Recorder, *Old);
  }
}

} // namespace __xsan::desan

namespace __xsan {
namespace {

// DeSan deliberately keeps these interceptor bodies under __xsan with names
// different from their LLVM interceptees.  We do not rely on DSO
// injection/interposition for interception; recording is enabled only when
// needed by planting trampolines at run time, so matching LLVM's mangled names
// is unnecessary and would hide that these are patch entry points.
void removeFromParent(llvm::Instruction *Self);
compat::InstructionEraseIterator eraseFromParent(llvm::Instruction *Self);
void replaceAllUsesWith(llvm::Value *Self, llvm::Value *V);
void ReplaceInstWithInst(llvm::Instruction *From, llvm::Instruction *To);

static ManualInterception RemoveFromParentInterceptee(
    &removeFromParent, &llvm::Instruction::removeFromParent,
    desan::InterceptionTargetProcesses, desan::InterceptionGroup);

void removeFromParent(llvm::Instruction *Self) {
  desan::recordDeleteEvent(Self);
  RemoveFromParentInterceptee(Self);
}

static ManualInterception EraseFromParentInterceptee(
    &eraseFromParent, &llvm::Instruction::eraseFromParent,
    desan::InterceptionTargetProcesses, desan::InterceptionGroup);

compat::InstructionEraseIterator eraseFromParent(llvm::Instruction *Self) {
  desan::recordDeleteEvent(Self);
  if (desan::isRecording() && desan::hasUserMark(*Self)) {
    auto Next = std::next(Self->getIterator());
    RemoveFromParentInterceptee(Self);
    return Next;
  }
  return EraseFromParentInterceptee(Self);
}

static ManualInterception ReplaceAllUsesWithInterceptee(
    &replaceAllUsesWith, &llvm::Value::replaceAllUsesWith,
    desan::InterceptionTargetProcesses, desan::InterceptionGroup);

void replaceAllUsesWith(llvm::Value *Self, llvm::Value *V) {
  // RAUW can recurse when a Constant user needs to rebuild itself.
  // For example, replacing an operand of a Global may trigger
  // Constant::handleOperandChange(), which then performs another RAUW on the
  // rebuilt Constant/Global. We only record the outermost RAUW semantic event;
  // recursive RAUW calls reuse the same interceptor critical section.
  if (!ReplaceAllUsesWithInterceptee.isRecursive())
    desan::recordReplaceEvent(Self, V);
  ReplaceAllUsesWithInterceptee(Self, V);
}

static ManualInterception ReplaceInstWithInstInterceptee(
    &ReplaceInstWithInst,
    /* select one of the overloadings */
    static_cast<decltype(&ReplaceInstWithInst)>(&llvm::ReplaceInstWithInst),
    desan::InterceptionTargetProcesses, desan::InterceptionGroup);

void ReplaceInstWithInst(llvm::Instruction *From, llvm::Instruction *To) {
  if (desan::isRecording() && From && To) {
    if (desan::hasUserMark(*From)) {
      if (!To->getDebugLoc())
        To->setDebugLoc(From->getDebugLoc());
      compat::insertInstructionBefore(To, From);
      From->replaceAllUsesWith(To);
      desan::recordDeleteEvent(From);
      RemoveFromParentInterceptee(From);
      return;
    }
  }
  ReplaceInstWithInstInterceptee(From, To);
}

// Keep the following comments for potential future interception of
// BasicBlock::eraseFromParent

// static __xsan::Interception
//     BasicBlockRemoveFromParentInterception(&llvm::BasicBlock::removeFromParent,
//                                           {"clang", "clang++"});

// static __xsan::Interception
//     BasicBlockEraseFromParentInterception(&llvm::BasicBlock::eraseFromParent,
//                                          {"clang", "clang++"});

// SymbolTableList<BasicBlock>::iterator BasicBlock::eraseFromParent() {
//   if (desan::isRecording() && !getParent()->isDeclaration()) {
//     auto Next = std::next(getIterator());
//     BasicBlockRemoveFromParentInterception(this);
//     return Next;
//   }
//   return BasicBlockEraseFromParentInterception(this);
// }

} // namespace
} // namespace __xsan
