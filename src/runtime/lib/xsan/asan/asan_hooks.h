#pragma once

#include "../xsan_hooks_default.h"
#include "../xsan_hooks_types.h"
#include "../xsan_stack_interface.h"
#include "asan_interface_xsan.h"

namespace __asan {

class AsanThread;

using AsanContext = ::__xsan::DefaultContext<__xsan::XsanHooksSanitizer::Asan>;
struct AsanHooksThread {
  AsanThread *asan_thread = nullptr;
};

PSEUDO_MACRO void AccessMemoryRange(const AsanContext *ctx, const uptr offset,
                                    uptr size, bool isWrite,
                                    const char *func_name) {
  uptr bad = 0;
  if (UNLIKELY(offset > offset + size)) {
    UNINITIALIZED BufferedStackTrace stack;
    __xsan::GetStackTraceFatalHere(stack);
    ReportStringFunctionSizeOverflow(offset, size, &stack);
  }
  if (UNLIKELY(!__asan::AsanQuickCheckForUnpoisonedRegion_(offset, size)) &&
      (bad = __asan_region_is_poisoned(offset, size))) {
    bool suppressed = false;
    if (func_name) {
      suppressed = __asan::IsInterceptorSuppressed(func_name);
      if (!suppressed && __asan::HaveStackTraceBasedSuppressions()) {
        UNINITIALIZED BufferedStackTrace stack;
        __xsan::GetStackTraceFatalHere(stack);
        suppressed = __asan::IsStackTraceSuppressed(&stack);
      }
    }
    if (!suppressed) {
      GET_CURRENT_PC_BP_SP;
      __asan::ReportGenericError(pc, bp, sp, bad, isWrite, size, 0, false);
    }
  }
}

struct AsanHooks : ::__xsan::DefaultHooks<AsanContext, AsanHooksThread> {
  using Context = AsanContext;
  using Thread = AsanHooksThread;

  // ------------------ Xsan-Initialization-Related Hooks ----------------
  static constexpr char name[] = "AddressSanitizer";

  static void InitFromXsan() {
    __xsan::ScopedSanitizerToolName tool_name(name);
    __asan::AsanInitFromXsan();
  }
  static void InitFromXsanLate() {
    __xsan::ScopedSanitizerToolName tool_name(name);
    __asan::AsanInitFromXsanLate();
  }
  ALWAYS_INLINE static ArrayRef<__xsan::NamedRange> NeededMapRanges() {
    static bool initialized = false;
    static __xsan::NamedRange map_ranges[1];
    if (!initialized) {
      // caller is thread safe, so we do not use atomic_bool
      initialized = true;
      // ref: asan_shadow_setup.cpp InitializeShadowMemory
      auto shadow_begin = AsanShadowOffset() - GetMmapGranularity();
      auto shadow_end =
          (__xsan::HiAppMemEnd() >> AsanShadowScale()) + AsanShadowOffset();
      map_ranges[0] = {{shadow_begin, shadow_end}, "asan shadow"};
    }
    return map_ranges;
  }
  // ---------------------- Memory Management Hooks -------------------
  /// As XSan uses ASan's heap allocator and fake stack directly, hence we don't
  /// need to invoke ASan's hooks here.
  // ---------------------- pthread-related hooks -----------------
  /// ASan 1) checks the correctness of main thread ID, 2) checks the init
  /// orders.
  static void OnPthreadCreate();
  // ---------------------- Special Function Hooks -----------------
  class ScopedAtExitHandler {
   public:
    ScopedAtExitHandler(uptr pc, const void *ctx);
    ~ScopedAtExitHandler();
  };

  static void vfork_parent_after_handle_sp(void *sp) {
    /// ASan will reset the shadow of [stk_bot, sp] to 0,
    /// eliminating the impact of the vfork child process.
    __asan_handle_vfork(sp);
  }

  static void OnForkBefore();
  static void OnForkAfter(bool is_child);
  static void BeforeDlopen(const char *filename, int flag);
  static void OnLongjmp(void *env, const char *fn_name, uptr pc) {
    __asan_handle_no_return();
  }
  ALWAYS_INLINE static void ClearShadowMemoryForContextStack(void *addr,
                                                             uptr size) {
    __asan::PoisonShadow(reinterpret_cast<uptr>(addr), size, 0);
  }
  // ---------------------- Flags Registration Hooks ---------------
  ALWAYS_INLINE static void InitializeFlags() {
    __xsan::ScopedSanitizerToolName tool_name("AddressSanitizer");
    __asan::InitializeFlags();
  }
  ALWAYS_INLINE static void SetCommonFlags(CommonFlags &cf) {
    __asan::SetCommonFlags(cf);
  }
  ALWAYS_INLINE static void ValidateFlags() { __asan::ValidateFlags(); }
  // ---------- Thread-Related Hooks --------------------------
  ALWAYS_INLINE static void SetThreadName(const char *name) {
    __asan::SetAsanThreadName(name);
  }
  /// Should be asanThreadRegistry().SetThreadNameByUserId(thread, name)
  /// But asan does not remember UserId's for threads (pthread_t);
  /// and remembers all ever existed threads, so the linear search by UserId
  /// can be slow.
  // static void SetThreadNameByUserId(uptr uid, const char *name) {}
  static Thread CreateMainThread();
  static Thread CreateThread(u32 parent_tid, uptr child_uid, StackTrace *stack,
                             const void *data, uptr data_size, bool detached);
  static void ChildThreadInit(Thread &thread, tid_t os_id);
  static void ChildThreadStart(Thread &thread, tid_t os_id);
  // Since XSan use ASan's resource, ASan should be manually destroyed by XSan
  // in the last. So `DestroyThread` is left empty and provide
  // `DestroyThreadReal` for XSan to call
  // static void DestroyThread(Thread &thread);
  static void DestroyThreadReal(Thread &thread);
  // ---------- Synchronization and File-Related Hooks ------------------------
  static void AfterMmap(const Context &ctx, void *res, uptr size, int fd);
  static void BeforeMunmap(const Context &ctx, void *addr, uptr size);
  // ---------- Generic Hooks in Interceptors ----------------
  ALWAYS_INLINE static void InitializeInterceptors() {
    __asan::InitializeAsanInterceptors();
  }
  PSEUDO_MACRO static void ReadRange(Context *ctx, const void *offset,
                                     uptr size, const char *func_name) {
    AccessMemoryRange(ctx, (uptr)offset, size, false, func_name);
  }
  PSEUDO_MACRO static void WriteRange(Context *ctx, const void *offset,
                                      uptr size, const char *func_name) {
    AccessMemoryRange(ctx, (uptr)offset, size, true, func_name);
  }
  PSEUDO_MACRO static void CommonReadRange(Context *ctx, const void *offset,
                                           uptr size, const char *func_name) {
    ReadRange(ctx, offset, size, func_name);
  }
  PSEUDO_MACRO static void CommonWriteRange(Context *ctx, const void *offset,
                                            uptr size, const char *func_name) {
    WriteRange(ctx, offset, size, func_name);
  }
  PSEUDO_MACRO static void CommonSyscallPreReadRange(const Context &ctx,
                                                     const void *offset,
                                                     uptr size,
                                                     const char *func_name) {
    AccessMemoryRange(&ctx, (uptr)offset, size, false, func_name);
  }
  PSEUDO_MACRO static void CommonSyscallPreWriteRange(const Context &ctx,
                                                      const void *offset,
                                                      uptr size,
                                                      const char *func_name) {
    AccessMemoryRange(&ctx, (uptr)offset, size, false, func_name);
  }
  PSEUDO_MACRO static void OnTwoRangesOverlap(const char *offset1, uptr size1,
                                              const char *offset2, uptr size2,
                                              const char *func_name) {
    UNINITIALIZED BufferedStackTrace stack;
    __xsan::GetStackTraceFatalHere(stack);
    bool suppressed = __asan::IsInterceptorSuppressed(func_name);
    if (!suppressed && __asan::HaveStackTraceBasedSuppressions()) {
      suppressed = __asan::IsStackTraceSuppressed(&stack);
    }
    if (!suppressed) {
      __asan::ReportStringFunctionMemoryRangesOverlap(func_name, offset1, size1,
                                                      offset2, size2, &stack);
    }
  }
  // ---------- xsan_interface-Related Hooks ----------------
  template <u32 ReadSize>
  static void __xsan_unaligned_read(uptr p);
  template <u32 WriteSize>
  static void __xsan_unaligned_write(uptr p);
  template <u32 ReadSize>
  static void __xsan_read(uptr p);
  template <u32 WriteSize>
  static void __xsan_write(uptr p);
};

}  // namespace __asan

// Register the hooks for Asan.
namespace __xsan {

template <>
struct XsanHooksSanitizerImpl<XsanHooksSanitizer::Asan> {
  using Hooks = __asan::AsanHooks;
};

}  // namespace __xsan
