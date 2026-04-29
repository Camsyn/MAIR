#pragma once

#include "../xsan_attribute.h"
#include "../xsan_hooks_default.h"
#include "../xsan_hooks_types.h"
#include "sanitizer_common/sanitizer_platform_limits_posix.h"
#include "sanitizer_common/sanitizer_stacktrace.h"
#include "tsan_interface_xsan.h"
#include "tsan_platform.h"
#include "tsan_rtl_extra.h"

namespace __tsan {

struct TsanContext {
  __tsan::ThreadState *thr_;
  uptr pc_;

  TsanContext() : thr_(nullptr), pc_(0) {}
  TsanContext(uptr pc) : thr_(__tsan::cur_thread_init()), pc_(pc) {}
};

struct TsanThread {
  __tsan::ThreadState *tsan_thread = nullptr;
  uptr tid = 0;
};

template <bool is_read>
PSEUDO_MACRO void AccessMemoryRange(const TsanContext *ctx, const void *_offset,
                                    uptr size) {
  uptr offset = (uptr)_offset;
  if (size == 0)
    return;
  if (ctx)
    __tsan::MemoryAccessRangeT<is_read>(ctx->thr_, ctx->pc_, offset, size);
  else
    __tsan::MemoryAccessRangeT<is_read>(__tsan::cur_thread_init(),
                                        GET_CURRENT_PC(), offset, size);
}

struct TsanHooks : ::__xsan::DefaultHooks<TsanContext, TsanThread> {
  using Context = TsanContext;
  using Thread = TsanThread;

  // ------------------ Xsan-Initialization-Related Hooks ----------------
  static constexpr char name[] = "ThreadSanitizer";

  static void InitFromXsanVeryEarly() { __tsan::TsanInitFromXsanVeryEarly(); }
  static void InitFromXsanEarly() { __tsan::InitializePlatformEarly(); }
  static void InitFromXsan() {
    __xsan::ScopedSanitizerToolName tool_name(name);
    __tsan::TsanInitFromXsan();
  }
  static void InitFromXsanLate() {
    __xsan::ScopedSanitizerToolName tool_name(name);
    __tsan::TsanInitFromXsanLate();
  }
  ALWAYS_INLINE static ArrayRef<__xsan::NamedRange> NeededMapRanges() {
    static bool initialized = false;
    static __xsan::NamedRange map_ranges[8];
#define APP2SHADOW(app)                                           \
  {{(uptr)__tsan::MemToShadow(app##MemBeg()),                     \
    (uptr)(__tsan::MemToShadow(app##MemEnd() - 1) + kShadowCnt)}, \
   "TSan Shadow (" #app ")"}
#define APP2META(app)                                  \
  {{(uptr)__tsan::MemToMeta(app##MemBeg()),            \
    (uptr)(__tsan::MemToMeta(app##MemEnd() - 1) + 1)}, \
   "TSan Meta (" #app ")"}
    if (!initialized) {
      // caller is thread safe, so we do not use atomic_bool
      initialized = true;
      map_ranges[0] = APP2SHADOW(LoApp);
      map_ranges[1] = APP2SHADOW(MidApp);
      map_ranges[2] = APP2SHADOW(HiApp);
      map_ranges[3] = APP2SHADOW(Heap);
      map_ranges[4] = APP2META(LoApp);
      map_ranges[5] = APP2META(MidApp);
      map_ranges[6] = APP2META(HiApp);
      map_ranges[7] = APP2META(Heap);
    }
#undef APP2SHADOW
#undef APP2META
    return map_ranges;
  }
  // ------------------ State-Related Hooks ----------------
  static void EnterSymbolizer() { __tsan::EnterSymbolizer(); }
  static void ExitSymbolizer() { __tsan::ExitSymbolizer(); }
  static bool ShouldIgnoreInterceptors(const Context &ctx);
  static bool ShouldIgnoreAllocFreeHook();
  /*
   Corresponding TSan's logic:

   class ScopedReportBase {
     ...
     ScopedIgnoreInterceptors ignore_interceptors_;
   };
   */
  static void EnterReport();
  static void ExitReport();

  static void OnAllocatorUnmap(uptr p, uptr size);
  static void OnXsanAllocHook(uptr ptr, uptr size, BufferedStackTrace *stack);
  static void OnXsanFreeHook(uptr ptr, uptr size, BufferedStackTrace *stack);
  static void OnXsanAllocFreeTailHook(uptr pc);
  static void OnFakeStackDestroy(uptr addr, uptr size);
  static void OnDtlsAlloc(uptr addr, uptr size);
  // ---------------------- Flags Registration Hooks ---------------
  ALWAYS_INLINE static void InitializeFlags() {
    __xsan::ScopedSanitizerToolName tool_name("ThreadSanitizer");
    __tsan::InitializeFlags();
  }
  static void SetCommonFlags(CommonFlags &cf);
  static void ValidateFlags();
  // ---------- Thread-Related Hooks --------------------------
  static void SetThreadName(const char *name);
  static void SetThreadNameByUserId(uptr uid, const char *name);
  static Thread CreateMainThread();
  static Thread CreateThread(u32 parent_tid, uptr child_uid, StackTrace *stack,
                             const void *data, uptr data_size, bool detached);
  static void ChildThreadInit(Thread &thread, tid_t os_id);
  // Tsan should be ready to process any possible thread-related events even
  // happened when other sub-sanitizers are starting, such as ASan calls
  // `pthread_getattr_np` -> `realloc` -> `__sanitizer_malloc_hook`, where
  // user may trigger thread-related events. So we need to make TSan start
  // first.
  // static void ChildThreadStart(Thread &thread, tid_t os_id);
  static void ChildThreadStartReal(Thread &thread, tid_t os_id);
  static void DestroyThread(Thread &thread);
  // ---------- Synchronization and File-Related Hooks ------------------------
  static void AfterMmap(const Context &ctx, void *res, uptr size, int fd);
  static void BeforeMunmap(const Context &ctx, void *addr, uptr size);

  static void FdAccess(const Context &ctx, int fd) {
    __tsan::FdAccess(ctx.thr_, ctx.pc_, fd);
  }

  static void FdPipeCreate(const Context &ctx, int fd0, int fd1) {
    __tsan::FdPipeCreate(ctx.thr_, ctx.pc_, fd0, fd1);
  }

  static void FdAcquire(const Context &ctx, int fd) {
    __tsan::FdAcquire(ctx.thr_, ctx.pc_, fd);
  }

  static void BeforeDlIteratePhdrCallback(const Context &ctx,
                                          __sanitizer_dl_phdr_info &info,
                                          SIZE_T size);
  static void AfterDlIteratePhdrCallback(const Context &ctx,
                                         __sanitizer_dl_phdr_info &info,
                                         SIZE_T size);
  /// TSan may spawn a background thread to recycle resource in pthread_create.
  /// What's more, TSan does not support starting new threads after
  /// multi-threaded fork.
  class ScopedPthreadJoin {
   public:
    ScopedPthreadJoin(const int &res, const Context &ctx, const void *th);
    ~ScopedPthreadJoin();

   private:
    const int &res_;
    const Context &ctx_;
    Tid tid_;
  };

  class ScopedPthreadDetach {
   public:
    ScopedPthreadDetach(const int &res, const Context &xsan_ctx,
                        const void *th);
    ~ScopedPthreadDetach();

   private:
    const int &res_;
    const Context &ctx_;
    Tid tid_;
  };

  class ScopedPthreadTryJoin {
   public:
    ScopedPthreadTryJoin(const int &res, const Context &xsan_ctx,
                         const void *th);
    ~ScopedPthreadTryJoin();

   private:
    uptr th_;
    const int &res_;
    const Context &ctx_;
    Tid tid_;
  };

  static void OnPthreadCreate();
  // ---------------------- Special Function Hooks -----------------
  class ScopedAtExitWrapper {
   public:
    ScopedAtExitWrapper(uptr pc, const void *ctx);
    ~ScopedAtExitWrapper();
  };
  class ScopedAtExitHandler {
   public:
    ScopedAtExitHandler(uptr pc, const void *ctx);
    ~ScopedAtExitHandler();
  };
  /*
  - (Parent) vfork_before
  - vfork
  - (Child) vfork_child_after
  - (Parent) vfork_parent_after
  */
  static void vfork_child_after() {
    // ----------  [Child] after vfork ----------------
    // We need to disable TSan for the vfork child process, because the child
    // process will inherit the same address space as the parent process.
    __tsan::DisableTsanForVfork();
  }
  static void vfork_parent_after() {
    // ----------  [Parent] after vfork ----------------
    // Must after '[Child] after vfork', as parent process will suspend until
    // child process `exit`/`exec`.

    // Due to the child process sharing the same address space as the parent
    // process, we need to recover TSan for the parent process after child
    // exits.
    __tsan::RecoverTsanAfterVforkParent();
  }

  static void OnForkBefore();
  static void OnForkAfter(bool is_child);
  static void OnLibraryLoaded(const char *filename, void *handle);
  static void OnLibraryUnloaded();
  static void OnLongjmp(void *env, const char *fn_name, uptr pc);
  // ---------- Generic Hooks in Interceptors ----------------
  ALWAYS_INLINE static void InitializeInterceptors() {
    __tsan::InitializeInterceptors();
  }
  PSEUDO_MACRO static void ReadRange(const Context *ctx, const void *offset,
                                     uptr size, const char *func_name) {
    if (TSAN_CHECK_GUARD_CONDIITON)
      return;
    AccessMemoryRange<true>(ctx, offset, size);
  }
  PSEUDO_MACRO static void WriteRange(const Context *ctx, const void *offset,
                                      uptr size, const char *func_name) {
    if (TSAN_CHECK_GUARD_CONDIITON)
      return;
    AccessMemoryRange<false>(ctx, offset, size);
  }
  PSEUDO_MACRO static void CommonReadRange(const Context *ctx,
                                           const void *offset, uptr size,
                                           const char *func_name) {
    ReadRange(ctx, offset, size, func_name);
  }
  PSEUDO_MACRO static void CommonWriteRange(const Context *ctx,
                                            const void *offset, uptr size,
                                            const char *func_name) {
    WriteRange(ctx, offset, size, func_name);
  }
  PSEUDO_MACRO static void CommonSyscallPreReadRange(const Context &ctx,
                                                     const void *offset,
                                                     uptr size,
                                                     const char *func_name) {
    ReadRange(&ctx, offset, size, func_name);
  }
  PSEUDO_MACRO static void CommonSyscallPreWriteRange(const Context &ctx,
                                                      const void *offset,
                                                      uptr size,
                                                      const char *func_name) {
    WriteRange(&ctx, offset, size, func_name);
  }
  // ---------- Xsan-Interface-Related Hooks ----------------
  template <u32 ReadSize>
  static void __xsan_unaligned_read(uptr p);
  template <u32 WriteSize>
  static void __xsan_unaligned_write(uptr p);
  template <u32 ReadSize>
  static void __xsan_read(uptr p);
  template <u32 WriteSize>
  static void __xsan_write(uptr p);
  // ---------- Func to use special scope ------------------------
  template <__xsan::ScopedFunc func>
  struct FuncScope {};

  // We miss atomic synchronization in getaddrinfo,
  // and can report false race between malloc and free
  // inside of getaddrinfo. So ignore memory accesses.
  template <>
  struct FuncScope<__xsan::ScopedFunc::getaddrinfo> {
    const Context ctx_;
    FuncScope() : ctx_(GET_CURRENT_PC()) {
      ThreadIgnoreBegin(ctx_.thr_, ctx_.pc_);
    }
    ~FuncScope() { ThreadIgnoreEnd(ctx_.thr_); }
  };
};

}  // namespace __tsan

// Register the hooks for Tsan.
namespace __xsan {

template <>
struct XsanHooksSanitizerImpl<XsanHooksSanitizer::Tsan> {
  using Hooks = __tsan::TsanHooks;
};

}  // namespace __xsan
