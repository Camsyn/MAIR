#include "PatchCore.h"
#include "Utils/MetaDataUtils.h"
#include "livepatch/record/SanitizingRecorder.h"
#include "xsan_llvm_compat.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Argument.h"
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

/// Manages the events and notify them when anchor updates.
template <typename EventT, typename AnchorT> class AnchoredEventStore {
public:
  using IndexList = llvm::SmallVector<size_t, 1>;

  void track(AnchorT Anchor, EventT &&Event) {
    const size_t EventIndex = Events.size();
    Events.push_back(std::move(Event));
    if (Anchor)
      AnchorMap[Anchor].push_back(EventIndex);
  }

  const EventT *getLastEventAnchoredBy(AnchorT Anchor) {
    if (!Anchor)
      return nullptr;
    auto It = AnchorMap.find(Anchor);
    if (It == AnchorMap.end() || It->second.empty())
      return nullptr;
    return &Events[It->second.back()];
  }

  void notifyAnchorChange(AnchorT OldAnchor, AnchorT NewAnchor) {
    if (NewAnchor == OldAnchor)
      return;

    auto It = AnchorMap.find(OldAnchor);
    if (It == AnchorMap.end())
      return;

    // Update the anchor
    for (size_t EventIndex : It->second)
      Events[EventIndex].update(OldAnchor, NewAnchor);

    // EventIndicesByAnchor[OldAnchor] --> EventIndicesByAnchor[NewAnchor]
    IndexList EventIndices = std::move(It->second);
    AnchorMap.erase(It);
    auto NewIt = AnchorMap.find(NewAnchor);
    if (NewIt == AnchorMap.end())
      AnchorMap.try_emplace(NewAnchor, std::move(EventIndices));
    else
      NewIt->second.append(EventIndices.begin(), EventIndices.end());
  }

  const std::vector<EventT> &getEvents() const { return Events; }

  void clear() {
    Events.clear();
    AnchorMap.clear();
  }

private:
  std::vector<EventT> Events;
  llvm::DenseMap<AnchorT, IndexList> AnchorMap;
};

namespace {

struct SanitizingRecorder {
  std::mutex Mutex;
  bool Active = false;
  AnchoredEventStore<DeleteEvent, const llvm::Instruction *> DeleteEvents;
  AnchoredEventStore<ReplaceEvent, const llvm::Value *> ReplaceEvents;
};

SanitizingRecorder &getRecorder() {
  static SanitizingRecorder Recorder;
  return Recorder;
}

void resetStatsLocked(SanitizingRecorder &Recorder) {
  Recorder.DeleteEvents.clear();
  Recorder.ReplaceEvents.clear();
}

bool shouldRecordLocked(const SanitizingRecorder &Recorder) {
  return Recorder.Active;
}

DeleteEvent buildDeleteEvent(llvm::Instruction &I) {
  // We need to also eliminate operands in case the operands can find the
  // deleted instruction through their user list.
  llvm::SmallVector<llvm::Value *, 4> Operands;
  Operands.reserve(I.getNumOperands());
  for (auto &Use : I.operands()) {
    Operands.push_back(Use.get());
    Use.set(nullptr);
  }
  return {&I, I.getParent(), I.getNextNode(), std::move(Operands)};
}

ReplaceEvent buildReplaceEvent(const llvm::Value &Old, const llvm::Value &New) {
  return {const_cast<llvm::Value *>(&Old), const_cast<llvm::Value *>(&New)};
}

bool isUserReplacee(const llvm::Value &Replacee) {
  return llvm::isa<llvm::Argument>(Replacee) ||
         (llvm::isa<llvm::Instruction>(Replacee) &&
          __xsan::UserIR::is(llvm::cast<llvm::Instruction>(Replacee)));
}

} // namespace

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

const std::vector<DeleteEvent> &getDeleteEvents() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.DeleteEvents.getEvents();
}

const std::vector<ReplaceEvent> &getReplaceEvents() {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  return Recorder.ReplaceEvents.getEvents();
}

void recordDeleteEvent(llvm::Instruction *I) {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  if (!shouldRecordLocked(Recorder) || !I || !I->getParent())
    return;

  if (!__xsan::UserIR::is(*I)) {
    // This non-user instruction was already an anchor, update all events
    // anchored to it. We don't do update for user instructions because
    // their order should be preserved and we can restore them unlike deleted
    // non-user instructions.
    Recorder.DeleteEvents.notifyAnchorChange(I, I->getNextNode());
  } else {
    auto Event = buildDeleteEvent(*I);
    auto *RawAnchor = Event.Anchor;
    // If some Inst2Removes has binded to the RawAnchor, we set the anchor of
    // the new event as the last Inst2Remove.
    if (auto *LastEvent =
            Recorder.DeleteEvents.getLastEventAnchoredBy(Event.Anchor))
      const_cast<llvm::Instruction *&>(Event.Anchor) = LastEvent->Inst2Remove;
    Recorder.DeleteEvents.track(RawAnchor, std::move(Event));
  }
}

void recordReplaceEvent(llvm::Value *Old, llvm::Value *New) {
  auto &Recorder = getRecorder();
  std::lock_guard<std::mutex> Lock(Recorder.Mutex);
  if (!shouldRecordLocked(Recorder) || !Old || !New)
    return;

  if (isUserReplacee(*Old))
    Recorder.ReplaceEvents.track(New, buildReplaceEvent(*Old, *New));
  else
    Recorder.ReplaceEvents.notifyAnchorChange(Old, New);
}

} // namespace __xsan::desan

namespace __xsan::desan {
namespace {

// DeSan deliberately keeps these interceptor bodies in the desan namespace with
// names different from their LLVM interceptees. We do not rely on DSO
// injection/interposition for interception; recording is enabled only when
// needed by planting trampolines at run time, so matching LLVM's mangled names
// is unnecessary and would hide that these are patch entry points.
void removeFromParent(llvm::Instruction *Self);
compat::InstructionEraseIterator eraseFromParent(llvm::Instruction *Self);
void replaceAllUsesWith(llvm::Value *Self, llvm::Value *V);
void ReplaceInstWithInst(llvm::Instruction *From, llvm::Instruction *To);

static __xsan::ManualInterception
    RemoveFromParentInterceptee(&removeFromParent,
                                &llvm::Instruction::removeFromParent,
                                InterceptionTargetProcesses, InterceptionGroup);

void removeFromParent(llvm::Instruction *Self) {
  recordDeleteEvent(Self);
  RemoveFromParentInterceptee(Self);
}

static __xsan::ManualInterception
    EraseFromParentInterceptee(&eraseFromParent,
                               &llvm::Instruction::eraseFromParent,
                               InterceptionTargetProcesses, InterceptionGroup);

compat::InstructionEraseIterator eraseFromParent(llvm::Instruction *Self) {
  recordDeleteEvent(Self);
  if (isRecording() && __xsan::UserIR::is(*Self)) {
    auto Next = std::next(Self->getIterator());
    RemoveFromParentInterceptee(Self);
    return Next;
  }
  return EraseFromParentInterceptee(Self);
}

static __xsan::ManualInterception ReplaceAllUsesWithInterceptee(
    &replaceAllUsesWith, &llvm::Value::replaceAllUsesWith,
    InterceptionTargetProcesses, InterceptionGroup);

void replaceAllUsesWith(llvm::Value *Self, llvm::Value *V) {
  // RAUW can recurse when a Constant user needs to rebuild itself.
  // For example, replacing an operand of a Global may trigger
  // Constant::handleOperandChange(), which then performs another RAUW on the
  // rebuilt Constant/Global. We only record the outermost RAUW semantic event;
  // recursive RAUW calls reuse the same interceptor critical section.
  if (!ReplaceAllUsesWithInterceptee.isRecursive())
    recordReplaceEvent(Self, V);
  ReplaceAllUsesWithInterceptee(Self, V);
}

static __xsan::ManualInterception ReplaceInstWithInstInterceptee(
    &ReplaceInstWithInst,
    /* select one of the overloadings */
    static_cast<decltype(&ReplaceInstWithInst)>(&llvm::ReplaceInstWithInst),
    InterceptionTargetProcesses, InterceptionGroup);

void ReplaceInstWithInst(llvm::Instruction *From, llvm::Instruction *To) {
  if (isRecording() && From && To) {
    if (__xsan::UserIR::is(*From)) {
      if (!To->getDebugLoc())
        To->setDebugLoc(From->getDebugLoc());
      compat::insertInstructionBefore(To, From);
      From->replaceAllUsesWith(To);
      recordDeleteEvent(From);
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
} // namespace __xsan::desan
