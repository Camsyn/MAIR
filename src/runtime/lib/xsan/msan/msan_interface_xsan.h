// For XSan to use MSan's API, while not introducing so many unrelated things.
#pragma once

/// Directly copied from somewhere of MSan to provide some interfaces to hooks.

#include "../xsan_platform.h"
#include "msan_chained_origin_depot.h"
#include "msan_flags.h"
#include "msan_interface_internal.h"
#include "msan_report.h"
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_stacktrace.h"

// ------------------ MSan TLS --------------------
// static constexpr int kMsanParamTlsSize = 800;
#define MSAN_PARAM_TLS_SIZE 800
extern THREADLOCAL u64 __msan_param_tls[MSAN_PARAM_TLS_SIZE / sizeof(u64)];
extern THREADLOCAL u32
    __msan_param_origin_tls[MSAN_PARAM_TLS_SIZE / sizeof(u32)];
#undef MSAN_PARAM_TLS_SIZE

extern "C" {

void __msan_unaligned_load2(uptr p);
void __msan_unaligned_load4(uptr p);
void __msan_unaligned_load8(uptr p);
void __msan_unaligned_store2(uptr p);
void __msan_unaligned_store4(uptr p);
void __msan_unaligned_store8(uptr p);

}  // extern "C"

namespace __msan {

using ::__sanitizer::StackTrace;
using ::__sanitizer::uptr;

#define MSAN_MEM_TO_SHADOW(p) ((p) ^ MAP_FIELD(kMSanShadowXor))
#define MSAN_SHADOW_TO_ORIGIN(p) ((p) + MAP_FIELD(kMSanShadowAdd))
#define MSAN_MEM_TO_ORIGIN(p) MSAN_SHADOW_TO_ORIGIN(MSAN_MEM_TO_SHADOW(p))

#define MSAN_CVT_FUNC(name, cvt)                              \
  XSAN_MAP_FUNC(uptr, name, (uptr p), (p)) { return cvt(p); } \
  template <typename T>                                       \
  ALWAYS_INLINE uptr name(T p) {                              \
    return name((uptr)p);                                     \
  }

#define MSAN_MAP_BEG(name, field, map) \
  XSAN_MAP_FUNC_VOID(uptr, name) { return map(MAP_FIELD(field)); }

#define MSAN_MAP_END(name, field, map) \
  XSAN_MAP_FUNC_VOID(uptr, name) { return map(MAP_FIELD(field) - 1) + 1; }

MSAN_MAP_BEG(LoShadowBeg, kLoAppMemBeg, MSAN_MEM_TO_SHADOW)
MSAN_MAP_END(LoShadowEnd, kLoAppMemEnd, MSAN_MEM_TO_SHADOW)
MSAN_MAP_BEG(MidShadowBeg, kMidAppMemBeg, MSAN_MEM_TO_SHADOW)
MSAN_MAP_END(MidShadowEnd, kMidAppMemEnd, MSAN_MEM_TO_SHADOW)
MSAN_MAP_BEG(HiShadowBeg, kHiAppMemBeg, MSAN_MEM_TO_SHADOW)
MSAN_MAP_END(HiShadowEnd, kHiAppMemEnd, MSAN_MEM_TO_SHADOW)
MSAN_MAP_BEG(HeapShadowBeg, kHeapMemBeg, MSAN_MEM_TO_SHADOW)
MSAN_MAP_END(HeapShadowEnd, kHeapMemEnd, MSAN_MEM_TO_SHADOW)

MSAN_MAP_BEG(LoOriginBeg, kLoAppMemBeg, MSAN_MEM_TO_ORIGIN)
MSAN_MAP_END(LoOriginEnd, kLoAppMemEnd, MSAN_MEM_TO_ORIGIN)
MSAN_MAP_BEG(MidOriginBeg, kMidAppMemBeg, MSAN_MEM_TO_ORIGIN)
MSAN_MAP_END(MidOriginEnd, kMidAppMemEnd, MSAN_MEM_TO_ORIGIN)
MSAN_MAP_BEG(HiOriginBeg, kHiAppMemBeg, MSAN_MEM_TO_ORIGIN)
MSAN_MAP_END(HiOriginEnd, kHiAppMemEnd, MSAN_MEM_TO_ORIGIN)
MSAN_MAP_BEG(HeapOriginBeg, kHeapMemBeg, MSAN_MEM_TO_ORIGIN)
MSAN_MAP_END(HeapOriginEnd, kHeapMemEnd, MSAN_MEM_TO_ORIGIN)

MSAN_CVT_FUNC(MemToShadow, MSAN_MEM_TO_SHADOW)
MSAN_CVT_FUNC(ShadowToOrigin, MSAN_SHADOW_TO_ORIGIN)
MSAN_CVT_FUNC(MemToOrigin, MSAN_MEM_TO_ORIGIN)

#undef MSAN_MAP_END
#undef MSAN_MAP_BEG
#undef MSAN_CVT_FUNC
#undef MSAN_MEM_TO_ORIGIN
#undef MSAN_SHADOW_TO_ORIGIN
#undef MSAN_MEM_TO_SHADOW

// ------------------ Check Functions --------------------

void CopyShadowAndOrigin(const void *dst, const void *src, uptr size,
                         StackTrace *stack);
void MoveShadowAndOrigin(const void *dst, const void *src, uptr size,
                         StackTrace *stack);
void PrintWarningWithOrigin(uptr pc, uptr bp, u32 origin);
void UnpoisonParam(uptr n);
void SetShadow(const void *ptr, uptr size, u8 value);
void SetOrigin(const void *dst, uptr size, u32 origin);

// ------------------- Interceptor Scope -------------------

extern THREADLOCAL int in_interceptor_scope;

struct InterceptorScope {
  ALWAYS_INLINE InterceptorScope() { ++in_interceptor_scope; }
  ALWAYS_INLINE ~InterceptorScope() { --in_interceptor_scope; }
};

ALWAYS_INLINE bool IsInInterceptorScope() { return in_interceptor_scope; }

// ------------------ Symbolizer and Unwinder ---------------

extern THREADLOCAL int is_in_symbolizer_or_unwinder;
ALWAYS_INLINE void EnterSymbolizerOrUnwider() {
  ++is_in_symbolizer_or_unwinder;
}
ALWAYS_INLINE void ExitSymbolizerOrUnwider() { --is_in_symbolizer_or_unwinder; }
ALWAYS_INLINE bool IsInSymbolizerOrUnwider() {
  return is_in_symbolizer_or_unwinder;
}

// ------------------ Signal Handler Scopers ----------------

class ScopedThreadLocalStateBackup {
 public:
  ScopedThreadLocalStateBackup() { Backup(); }
  ~ScopedThreadLocalStateBackup() { Restore(); }
  void Backup();
  void Restore();

 private:
  u64 va_arg_overflow_size_tls;
};

// ------------------ Init Msan --------------------

extern bool msan_init_is_running;
extern int msan_inited;
void InitializeFlags();
void InitializeInterceptors();
void MsanInitFromXsan();

// ------------------ At Exit --------------------
void MsanAtExit();

}  // namespace __msan
