#pragma once

#include "../xsan_hooks_default.h"
#include "../xsan_hooks_types.h"
#include "msan_interface_xsan.h"
#include "msan_thread.h"
#include "sanitizer_common/sanitizer_errno.h"

namespace __msan {

using MsanContext = ::__xsan::DefaultContext<__xsan::XsanHooksSanitizer::Msan>;

PSEUDO_MACRO static void CheckUnpoisoned(const void *_x, uptr n,
                                         const char *func_name) {
  if (__msan::IsInSymbolizerOrUnwider())
    return;
  const char *x = (const char *)_x;
  sptr offset = (sptr)__msan_test_shadow(x, n);
  if (offset >= 0 && __msan::flags()->report_umrs) {
    GET_CALLER_PC_BP;
    __msan::ReportUMRInsideAddressRange(func_name, x, n, offset);
    __msan::PrintWarningWithOrigin(pc, bp, __msan_get_origin(x + offset));
    if (__msan::flags()->halt_on_error) {
      Printf("Exiting\n");
      Die();
    }
  }
}

PSEUDO_MACRO static void CheckMemIntrinsicSizeParam() {
  // in __xsan_mem*, check whether the third param (i.e., size_t size) is
  // initialized
  SIZE_T shadow;
  u32 origin = __msan_get_track_origins() ? __msan_param_origin_tls[4] : 0;

  if constexpr (sizeof(SIZE_T) == 8) {
    shadow = __msan_param_tls[2];
    __msan_maybe_warning_8(shadow, origin);
  } else {
    internal_memcpy(&shadow, &__msan_param_tls[2], sizeof(SIZE_T));
    if constexpr (sizeof(SIZE_T) == 4) {
      __msan_maybe_warning_4(shadow, origin);
    } else if constexpr (sizeof(SIZE_T) == 2) {
      __msan_maybe_warning_2(shadow, origin);
    } else if constexpr (sizeof(SIZE_T) == 1) {
      __msan_maybe_warning_1(shadow, origin);
    }
  }
}

struct MsanHooks : ::__xsan::DefaultHooks<MsanContext, MsanThread> {
  using Context = MsanContext;
  using Thread = MsanThread;

  static void InitFromXsanVeryEarly() {
    CHECK(!msan_init_is_running);
    if (msan_inited)
      return;
    msan_init_is_running = 1;
  }
  static void InitFromXsan() {
    __xsan::ScopedSanitizerToolName tool_name("MemorySanitizer");
    __msan::MsanInitFromXsan();
  }
  static void InitFromXsanLate() {
    msan_init_is_running = 0;
    msan_inited = 1;
  }
  ALWAYS_INLINE static ArrayRef<__xsan::NamedRange> NeededMapRanges() {
    static bool initialized = false;
    static __xsan::NamedRange map_ranges[8];
    if (!initialized) {
      // caller is thread safe, so we do not use atomic_bool
      initialized = true;
      map_ranges[0] = {{LoShadowBeg(), LoShadowEnd()}, "msan shadow low"};
      map_ranges[1] = {{MidShadowBeg(), MidShadowEnd()}, "msan shadow mid"};
      map_ranges[2] = {{HiShadowBeg(), HiShadowEnd()}, "msan shadow high"};
      map_ranges[3] = {{HeapShadowBeg(), HeapShadowEnd()}, "msan shadow heap"};
      map_ranges[4] = {{LoOriginBeg(), LoOriginEnd()}, "msan origin low"};
      map_ranges[5] = {{MidOriginBeg(), MidOriginEnd()}, "msan origin mid"};
      map_ranges[6] = {{HiOriginBeg(), HiOriginEnd()}, "msan origin high"};
      map_ranges[7] = {{HeapOriginBeg(), HeapOriginEnd()}, "msan origin heap"};
    }
    return map_ranges;
  }

  static void OnAllocatorUnmap(uptr p, uptr size);
  static void OnXsanAllocHook(uptr ptr, uptr size, BufferedStackTrace *stack);
  static void OnXsanFreeHook(uptr ptr, uptr size, BufferedStackTrace *stack);
  /// Now MSan directly poison stack shadow via ReplacedAlloca API in pass.
  // ALWAYS_INLINE static void OnFakeStackAlloc(uptr addr, uptr size) {
  //   // We now let ASan tag its replaced alloca.
  //   __sanitizer::internal_memset((void *)MemToShadow(addr), 0xff, size);
  // }
  ALWAYS_INLINE static void OnDtlsAlloc(uptr addr, uptr size) {
    CommonInitRange(nullptr, (void *)addr, size);
  }

  static void OnLibraryLoaded(const char *filename, void *handle);
  ALWAYS_INLINE static void ClearShadowMemoryForContextStack(void *addr,
                                                             uptr size) {
    __msan_unpoison(addr, size);
  }

  ALWAYS_INLINE static void AtExit() { MsanAtExit(); }

  class ScopedAtExitHandler {
   public:
    ALWAYS_INLINE ScopedAtExitHandler(uptr pc, const void *ctx) {
      UnpoisonParam(1);
    }
    ALWAYS_INLINE ~ScopedAtExitHandler() {}
  };

  ALWAYS_INLINE static void OnForkBefore() { ChainedOriginDepotBeforeFork(); }
  ALWAYS_INLINE static void OnForkAfter(bool is_child) {
    ChainedOriginDepotAfterFork(is_child);
  }

  ALWAYS_INLINE static void OnEnterUnwind() { EnterSymbolizerOrUnwider(); }
  ALWAYS_INLINE static void OnExitUnwind() { ExitSymbolizerOrUnwider(); }

  ALWAYS_INLINE static bool RequireStackTraces() {
    return __msan_get_track_origins() > 1;
  }
  static int RequireStackTracesSize();

  ALWAYS_INLINE static void InitializeFlags() {
    __xsan::ScopedSanitizerToolName tool_name("MemorySanitizer");
    __msan::InitializeFlags();
  }
  ALWAYS_INLINE static void SetCommonFlags(CommonFlags &cf) {
    cf.handle_ioctl = true;
    // cf.check_printf = false;
  }

  static void ChildThreadInit(Thread &thread, tid_t os_id);
  static void ChildThreadStart(Thread &thread, tid_t os_id);
  static void DestroyThread(Thread &thread);

  ALWAYS_INLINE static void AfterMmap(const Context &ctx, void *res, uptr size,
                                      int fd) {
    __msan_unpoison(res, size);
  }

  ALWAYS_INLINE static void InitializeInterceptors() {
    __msan::InitializeInterceptors();
  }
  PSEUDO_MACRO static void UseRange(const Context *ctx, const void *_x, uptr n,
                                    const char *func_name) {
    if (in_interceptor_scope)
      return;
    CheckUnpoisoned(_x, n, func_name);
  }
  PSEUDO_MACRO static void CopyRange(const Context *ctx, const void *dst,
                                     const void *src, uptr size,
                                     BufferedStackTrace &stack) {
    __msan::CopyShadowAndOrigin(dst, src, size, &stack);
  }
  PSEUDO_MACRO static void MoveRange(const Context *ctx, const void *dst,
                                     const void *src, uptr size,
                                     BufferedStackTrace &stack) {
    __msan::MoveShadowAndOrigin(dst, src, size, &stack);
  }
  PSEUDO_MACRO static void InitRange(const Context *ctx, const void *offset,
                                     uptr size) {
    if (FuncScope<__xsan::ScopedFunc::xsan_memintrinsic>::
            in_xsan_memintrinsic_scope) {
      // in __xsan_memset, use the second param as the value to be set
      u8 shadow;
      internal_memcpy(&shadow, &__msan_param_tls[1], sizeof(u8));
      if (!shadow) {
        __msan_unpoison(offset, size);
      } else {
        __msan::SetShadow(offset, size, shadow);
        if (__msan_get_track_origins()) {
          // the same offset as the shadow in __msan_param_tls
          u32 origin = __msan_param_origin_tls[2];
          __msan::SetOrigin(offset, size, origin);
        }
      }
    } else {
      __msan_unpoison(offset, size);
    }
  }
  PSEUDO_MACRO static void CommonReadRange(const Context *ctx,
                                           const void *offset, uptr size,
                                           const char *func_name) {
    if (FuncScope<__xsan::ScopedFunc::common>::saved_scope)
      return;
    CheckUnpoisoned(offset, size, func_name);
  }
  PSEUDO_MACRO
  static void CommonWriteRange(const Context *ctx, const void *offset,
                               uptr size, const char *func_name) {
    InitRange(ctx, offset, size);
  }
  PSEUDO_MACRO static void CommonUnpoisonParam(uptr count) {
    __msan::UnpoisonParam(count);
  }
  PSEUDO_MACRO static void CommonInitRange(const Context *ctx,
                                           const void *offset, uptr size) {
    InitRange(ctx, offset, size);
  }
  PSEUDO_MACRO static void CommonSyscallPreReadRange(const Context &ctx,
                                                     const void *offset,
                                                     uptr size,
                                                     const char *func_name) {
    UseRange(&ctx, offset, size, func_name);
  }
  PSEUDO_MACRO static void CommonSyscallPostWriteRange(const Context &ctx,
                                                       const void *offset,
                                                       uptr size,
                                                       const char *func_name) {
    InitRange(&ctx, offset, size);
  }

  template <s32 ReadSize>
  static void __xsan_unaligned_read(uptr p);
  template <s32 WriteSize>
  static void __xsan_unaligned_write(uptr p);

  template <__xsan::ScopedFunc func>
  struct FuncScope {};

  template <>
  struct FuncScope<__xsan::ScopedFunc::calloc> {
    THREADLOCAL static int in_calloc_scope;
    FuncScope() { in_calloc_scope++; }
    ~FuncScope() { in_calloc_scope--; }
  };

  template <>
  struct FuncScope<__xsan::ScopedFunc::strdup> {
    InterceptorScope interceptor_scope;
  };

  template <>
  struct FuncScope<__xsan::ScopedFunc::common> {
    THREADLOCAL static int saved_scope;
    FuncScope() {
      saved_scope = in_interceptor_scope++;
      __msan_unpoison(__errno_location(), sizeof(int));
    }
    ~FuncScope() { in_interceptor_scope--; }
  };

  template <>
  struct FuncScope<__xsan::ScopedFunc::signal> {
    ScopedThreadLocalStateBackup stlsb;
    FuncScope();
    ~FuncScope();
  };

  template <>
  struct FuncScope<__xsan::ScopedFunc::getaddrinfo> {
    FuncScope<__xsan::ScopedFunc::common> common_scope;
  };

  template <>
  struct FuncScope<__xsan::ScopedFunc::xsan_memintrinsic> {
    THREADLOCAL static int in_xsan_memintrinsic_scope;
    PSEUDO_MACRO FuncScope() {
      in_xsan_memintrinsic_scope++;
      // For user's memcpy/memmove/memset, we should verify the initialization of
      // the size param
      CheckMemIntrinsicSizeParam();
    }
    ~FuncScope() { in_xsan_memintrinsic_scope--; }
  };
};

}  // namespace __msan

// Register the hooks for Msan.
namespace __xsan {

template <>
struct XsanHooksSanitizerImpl<XsanHooksSanitizer::Msan> {
  using Hooks = __msan::MsanHooks;
};

}  // namespace __xsan
