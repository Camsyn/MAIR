#pragma once

#include <array>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char *const XSanDeSanUserInstMDKind = "xsan.user";
static constexpr std::array<const char *, 3> TargetProcesses{"clang", "clang++",
                                                             "opt"};

typedef enum XSanDeSanStageKind {
  XSAN_DESAN_STAGE_NONE = 0,
  XSAN_DESAN_STAGE_UBSAN = 1,
  XSAN_DESAN_STAGE_ASAN = 2,
  XSAN_DESAN_STAGE_TSAN = 3,
  XSAN_DESAN_STAGE_MSAN = 4,
  XSAN_DESAN_STAGE_RESAN = 5,
} XSanDeSanStageKind;

typedef enum XSanDeSanSanitizerMask {
  XSAN_DESAN_SANITIZER_ASAN = 1u << 0,
  XSAN_DESAN_SANITIZER_TSAN = 1u << 1,
  XSAN_DESAN_SANITIZER_MSAN = 1u << 2,
} XSanDeSanSanitizerMask;

#define XSAN_DESAN_INTERCEPTION_GROUP "desan"

typedef enum XSanDeSanDeleteAnchorKind {
  XSAN_DESAN_DELETE_ANCHOR_BB = 0,
  XSAN_DESAN_DELETE_ANCHOR_INST = 1,
} XSanDeSanDeleteAnchorKind;

typedef struct XSanDeSanDeleteEvent {
  uint64_t deleted_value;
  uint64_t anchor_ptr;
  uint32_t anchor_kind;
  uint32_t reserved;
} XSanDeSanDeleteEvent;

typedef struct XSanDeSanDeleteOperand {
  uint64_t value;
} XSanDeSanDeleteOperand;

const static XSanDeSanDeleteOperand XSanDeSanDeleteOperandNull = {0};

typedef struct XSanDeSanReplaceEvent {
  uint64_t user_value;
  uint64_t replace_value;
} XSanDeSanReplaceEvent;

typedef struct XSanDeSanDeleteBlock {
  uint64_t func_ptr;
  uint64_t block_ptr;
} XSanDeSanDeleteBlock;

void __xsan_desan_reset_session(void);
void __xsan_desan_begin_stage(uint32_t stage_kind);
void __xsan_desan_end_stage(void);
int __xsan_desan_is_stage_active(void);
uint64_t __xsan_desan_get_delete_event_count(void);
uint64_t __xsan_desan_copy_delete_events(XSanDeSanDeleteEvent *out_events,
                                         uint64_t capacity);
uint64_t __xsan_desan_get_delete_operands_count(void);
uint64_t __xsan_desan_copy_delete_operands(XSanDeSanDeleteOperand *out_operands,
                                           uint64_t capacity);
uint64_t __xsan_desan_get_replace_event_count(void);
uint64_t __xsan_desan_copy_replace_events(XSanDeSanReplaceEvent *out_events,
                                          uint64_t capacity);
uint64_t __xsan_desan_get_delete_block_count(void);
uint64_t __xsan_desan_copy_delete_blocks(XSanDeSanDeleteBlock *out_blocks,
                                         uint64_t capacity);

#ifdef __cplusplus
}
#endif
