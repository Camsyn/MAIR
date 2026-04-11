#pragma once

/// Directly copied from somewhere of ASan to provide some interfaces to hooks.
/// E.g., AsanQuickCheckForUnpoisonedRegion_ comes from QuickCheckForUnpoisonedRegion
/// in asan_interceptors_memintrinsics.h

#include "../xsan_platform.h"
#include "asan_interface_internal.h"
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_stacktrace.h"

extern "C" {
void *__asan_extra_spill_area();
void __asan_unaligned_load2(uptr p);
void __asan_unaligned_load4(uptr p);
void __asan_unaligned_load8(uptr p);
void __asan_unaligned_store2(uptr p);
void __asan_unaligned_store4(uptr p);
void __asan_unaligned_store8(uptr p);
}

namespace __asan {

using ::__sanitizer::StackTrace;
using ::__sanitizer::uptr;

XSAN_MAP_FIELD_FUNC(AsanShadowOffset, kAsanShadowOffset)
XSAN_MAP_FIELD_FUNC(AsanShadowScale, kAsanShadowScale)

ALWAYS_INLINE uptr AsanShadowGranularity() {
  return (1ULL << AsanShadowScale());
}

template <typename T>
ALWAYS_INLINE uptr MemToShadow(T mem) {
  return ((uptr)mem >> AsanShadowScale()) + AsanShadowOffset();
}

static inline bool AddressIsPoisoned_(uptr a) {
  // PROFILE_ASAN_MAPPING();
  const uptr kAccessSize = 1;
  u8 *shadow_address = (u8 *)MemToShadow(a);
  s8 shadow_value = *shadow_address;
  if (shadow_value) {
    u8 last_accessed_byte =
        (a & (AsanShadowGranularity() - 1)) + kAccessSize - 1;
    return (last_accessed_byte >= shadow_value);
  }
  return false;
}

// Return true if we can quickly decide that the region is unpoisoned.
// We assume that a redzone is at least 16 bytes.
static inline bool AsanQuickCheckForUnpoisonedRegion_(uptr beg, uptr size) {
  if (UNLIKELY(size == 0 || size > sizeof(uptr) * AsanShadowGranularity()))
    return !size;

  uptr last = beg + size - 1;
  uptr shadow_first = MemToShadow(beg);
  uptr shadow_last = MemToShadow(last);
  uptr uptr_first = RoundDownTo(shadow_first, sizeof(uptr));
  uptr uptr_last = RoundDownTo(shadow_last, sizeof(uptr));
  if (LIKELY(((*reinterpret_cast<const uptr *>(uptr_first) |
               *reinterpret_cast<const uptr *>(uptr_last)) == 0)))
    return true;
  u8 shadow = AddressIsPoisoned_(last);
  for (; shadow_first < shadow_last; ++shadow_first)
    shadow |= *((u8 *)shadow_first);
  return !shadow;
}

void PoisonShadow(uptr addr, uptr size, u8 value);

bool IsInterceptorSuppressed(const char *interceptor_name);
bool HaveStackTraceBasedSuppressions();
bool IsStackTraceSuppressed(const StackTrace *stack);
void ReportGenericError(uptr pc, uptr bp, uptr sp, uptr addr, bool is_write,
                        uptr access_size, u32 exp, bool fatal);
void ReportStringFunctionSizeOverflow(uptr offset, uptr size,
                                      BufferedStackTrace *stack);
void ReportStringFunctionMemoryRangesOverlap(const char *function,
                                             const char *offset1, uptr length1,
                                             const char *offset2, uptr length2,
                                             BufferedStackTrace *stack);
void InitializeFlags();
void ValidateFlags();
void SetCommonFlags(CommonFlags &cf);
void SetAsanThreadName(const char *name);

// Initialization before xsan_is_running = false;
void AsanInitFromXsan();
// Initialization after xsan_is_running = false;
void AsanInitFromXsanLate();

void InitializeAsanInterceptors();
}  // namespace __asan
