#include "XSanDeSanRuntime.h"

#include <dlfcn.h>
#include <vector>

namespace __xsan::desan {
namespace {

using ResetSessionFn = decltype(&__xsan_desan_reset_session);
using BeginStageFn = decltype(&__xsan_desan_begin_stage);
using EndStageFn = decltype(&__xsan_desan_end_stage);
using IsStageActiveFn = decltype(&__xsan_desan_is_stage_active);
using GetDeleteEventCountFn = decltype(&__xsan_desan_get_delete_event_count);
using CopyDeleteEventsFn = decltype(&__xsan_desan_copy_delete_events);
using GetDeleteOperandsCountFn =
    decltype(&__xsan_desan_get_delete_operands_count);
using CopyDeleteOperandsFn = decltype(&__xsan_desan_copy_delete_operands);
using GetReplaceEventCountFn = decltype(&__xsan_desan_get_replace_event_count);
using CopyReplaceEventsFn = decltype(&__xsan_desan_copy_replace_events);
using GetDeleteBlockCountFn = decltype(&__xsan_desan_get_delete_block_count);
using CopyDeleteBlocksFn = decltype(&__xsan_desan_copy_delete_blocks);

template <typename FnTy> FnTy resolve(const char *Name) {
  return reinterpret_cast<FnTy>(dlsym(RTLD_DEFAULT, Name));
}

ResetSessionFn getResetSessionFn() {
  static auto *Fn = resolve<ResetSessionFn>("__xsan_desan_reset_session");
  return Fn;
}

BeginStageFn getBeginStageFn() {
  static auto *Fn = resolve<BeginStageFn>("__xsan_desan_begin_stage");
  return Fn;
}

EndStageFn getEndStageFn() {
  static auto *Fn = resolve<EndStageFn>("__xsan_desan_end_stage");
  return Fn;
}

IsStageActiveFn getIsStageActiveFn() {
  static auto *Fn = resolve<IsStageActiveFn>("__xsan_desan_is_stage_active");
  return Fn;
}

GetDeleteEventCountFn getDeleteEventCountFn() {
  static auto *Fn =
      resolve<GetDeleteEventCountFn>("__xsan_desan_get_delete_event_count");
  return Fn;
}

CopyDeleteEventsFn getCopyDeleteEventsFn() {
  static auto *Fn =
      resolve<CopyDeleteEventsFn>("__xsan_desan_copy_delete_events");
  return Fn;
}

GetDeleteOperandsCountFn getDeleteOperandsCountFn() {
  static auto *Fn = resolve<GetDeleteOperandsCountFn>(
      "__xsan_desan_get_delete_operands_count");
  return Fn;
}

CopyDeleteOperandsFn getCopyDeleteOperandsFn() {
  static auto *Fn =
      resolve<CopyDeleteOperandsFn>("__xsan_desan_copy_delete_operands");
  return Fn;
}

GetReplaceEventCountFn getReplaceEventCountFn() {
  static auto *Fn =
      resolve<GetReplaceEventCountFn>("__xsan_desan_get_replace_event_count");
  return Fn;
}

CopyReplaceEventsFn getCopyReplaceEventsFn() {
  static auto *Fn =
      resolve<CopyReplaceEventsFn>("__xsan_desan_copy_replace_events");
  return Fn;
}

GetDeleteBlockCountFn getDeleteBlockCountFn() {
  static auto *Fn =
      resolve<GetDeleteBlockCountFn>("__xsan_desan_get_delete_block_count");
  return Fn;
}

CopyDeleteBlocksFn getCopyDeleteBlocksFn() {
  static auto *Fn =
      resolve<CopyDeleteBlocksFn>("__xsan_desan_copy_delete_blocks");
  return Fn;
}

DeleteEvent convert(const XSanDeSanDeleteEvent &Event,
                    const XSanDeSanDeleteOperand *(&Operands)) {
  DeleteEvent Out;
  Out.DeletedValue = reinterpret_cast<llvm::Instruction *>(Event.deleted_value);
  if (Event.anchor_kind == XSAN_DESAN_DELETE_ANCHOR_BB) {
    Out.Anchor = reinterpret_cast<llvm::BasicBlock *>(Event.anchor_ptr);
  } else {
    Out.Anchor = reinterpret_cast<llvm::Instruction *>(Event.anchor_ptr);
  }
  while (Operands->value != XSanDeSanDeleteOperandNull.value) {
    Out.Operands.push_back(reinterpret_cast<llvm::Value *>(Operands->value));
    ++Operands;
  }
  ++Operands;

  return Out;
}

ReplaceEvent convert(const XSanDeSanReplaceEvent &Event) {
  ReplaceEvent Out;
  Out.UserValue = reinterpret_cast<llvm::Value *>(Event.user_value);
  Out.ReplaceValue = reinterpret_cast<llvm::Value *>(Event.replace_value);
  return Out;
}

DeleteBlock convert(const XSanDeSanDeleteBlock &Event) {
  DeleteBlock Out;
  Out.Func = reinterpret_cast<llvm::Function *>(Event.func_ptr);
  Out.Block = reinterpret_cast<llvm::BasicBlock *>(Event.block_ptr);
  return Out;
}

} // namespace

bool isRuntimeAvailable() {
  return getResetSessionFn() && getBeginStageFn() && getEndStageFn() &&
         getIsStageActiveFn() && getDeleteEventCountFn() &&
         getCopyDeleteEventsFn() && getDeleteOperandsCountFn() &&
         getCopyDeleteOperandsFn() && getReplaceEventCountFn() &&
         getCopyReplaceEventsFn() && getDeleteBlockCountFn() &&
         getCopyDeleteBlocksFn();
}

void resetSession() {
  if (auto *Fn = getResetSessionFn())
    Fn();
}

void beginStage(StageKind Stage) {
  if (auto *Fn = getBeginStageFn())
    Fn(static_cast<uint32_t>(Stage));
}

void endStage() {
  if (auto *Fn = getEndStageFn())
    Fn();
}

const char *getStageName(StageKind Stage) {
  switch (Stage) {
  case StageKind::UBSan:
    return "ubsan";
  case StageKind::ASan:
    return "asan";
  case StageKind::TSan:
    return "tsan";
  case StageKind::MSan:
    return "msan";
  case StageKind::ReSan:
    return "resan";
  case StageKind::None:
  default:
    return "none";
  }
}

std::vector<DeleteEvent> getDeleteEvents() {
  auto *GetCount = getDeleteEventCountFn();
  auto *Copy = getCopyDeleteEventsFn();
  auto *GetOperandsCount = getDeleteOperandsCountFn();
  auto *CopyOperands = getCopyDeleteOperandsFn();
  if (!GetCount || !Copy || !GetOperandsCount || !CopyOperands)
    return {};

  const uint64_t Count = GetCount();
  const uint64_t OperandsCount = GetOperandsCount();
  if (Count == 0)
    return {};

  std::vector<XSanDeSanDeleteEvent> Raw(Count);
  std::vector<XSanDeSanDeleteOperand> OperandsRaw(OperandsCount);
  Copy(Raw.data(), Count);
  CopyOperands(OperandsRaw.data(), OperandsCount);

  std::vector<DeleteEvent> Events;
  Events.reserve(Count);
  const XSanDeSanDeleteOperand *CurrentOperand = OperandsRaw.data();
  for (const auto &Event : Raw)
    Events.push_back(convert(Event, CurrentOperand));
  return Events;
}

std::vector<ReplaceEvent> getReplaceEvents() {
  auto *GetCount = getReplaceEventCountFn();
  auto *Copy = getCopyReplaceEventsFn();
  if (!GetCount || !Copy)
    return {};

  const uint64_t Count = GetCount();
  if (Count == 0)
    return {};

  std::vector<XSanDeSanReplaceEvent> Raw(Count);
  Copy(Raw.data(), Count);

  std::vector<ReplaceEvent> Events;
  Events.reserve(Count);
  for (const auto &Event : Raw)
    Events.push_back(convert(Event));
  return Events;
}

std::vector<DeleteBlock> getDeleteBlocks() {
  auto *GetCount = getDeleteBlockCountFn();
  auto *Copy = getCopyDeleteBlocksFn();
  if (!GetCount || !Copy)
    return {};

  const uint64_t Count = GetCount();
  if (Count == 0)
    return {};

  std::vector<XSanDeSanDeleteBlock> Raw(Count);
  Copy(Raw.data(), Count);

  std::vector<DeleteBlock> Events;
  Events.reserve(Count);
  for (const auto &Event : Raw)
    Events.push_back(convert(Event));
  return Events;
}

} // namespace __xsan::desan
