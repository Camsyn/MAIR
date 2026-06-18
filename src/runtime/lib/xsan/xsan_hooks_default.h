//===-- xsan_hooks_default.h ------------------------------------------*- C++
//-*-===//
//
// This file is a part of XSan, an composition of several sanitizers.
//
// This file contains the defualt implementation of the hooks
//===----------------------------------------------------------------------===//
#pragma once

#include "interception/interception.h"
#include "sanitizer_common/sanitizer_array_ref.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_platform_limits_posix.h"
#include "xsan_attribute.h"
#include "xsan_hooks_types.h"

namespace __xsan {

// The use of templates here ensures that the `Context` class for sanitizers
// using the default context is not the same class. This is necessary because
// implicit type conversion rules need to be defined in bulk. Without templates,
// the `Context` for sanitizers using the default context would be the same
// class, which would lead to duplicate definitions of conversion rules.
template <XsanHooksSanitizer san>
struct DefaultContext {
  ALWAYS_INLINE DefaultContext() {};
  ALWAYS_INLINE DefaultContext(uptr pc) {};
};

template <XsanHooksSanitizer san>
struct DefaultThread {};

template <typename Context, typename Thread>
struct DefaultHooks {
  using BufferedStackTrace = ::__sanitizer::BufferedStackTrace;
  using u32 = ::__sanitizer::u32;
  using uptr = ::__sanitizer::uptr;

  // ---------- Xsan-Initialization-Related Hooks ----------------
  /// Before any other initialization.
  /// Used to initialize state of sub-santizers, e.g., Context of TSan.
  ALWAYS_INLINE static void InitFromXsanVeryEarly() {}
  /// After flags initialization, before any other initialization.
  ALWAYS_INLINE static void InitFromXsanEarly() {}
  ALWAYS_INLINE static void InitFromXsan() {}
  /// Almost after all is done, e.g., flags, memory, allocator, threads, etc.
  ALWAYS_INLINE static void InitFromXsanLate() {}
  // Return the shadow's mappings designated for sub-sanitizers.
  ALWAYS_INLINE static __sanitizer::ArrayRef<NamedRange> NeededMapRanges() {
    return {};
  }
  // ---------- End of Xsan-Initialization-Related Hooks ----------------

  // ----------- State/Ignoration Management Hooks -----------
  /*
   Manage and notify the following states:
    - if XSan is in internal
    - if XSan is in symbolizer
    - if XSan should ignore interceptors
    - the exit code of XSan
  */
  ALWAYS_INLINE static void EnterSymbolizer() {}
  ALWAYS_INLINE static void ExitSymbolizer() {}
  /*
   The sub-sanitizers implement the following ignore predicates to ignore
    - Interceptors
    - Allocation/Free Hooks
   which are shared by all sub-sanitizers.
 */
  ALWAYS_INLINE static bool ShouldIgnoreInterceptors(const Context &ctx) {
    return false;
  }
  ALWAYS_INLINE static bool ShouldIgnoreAllocFreeHook() { return false; }

  // Related to sanitizer error reporting
  ALWAYS_INLINE static void EnterReport() {}
  ALWAYS_INLINE static void ExitReport() {}
  // ---------- End of State Management Hooks -----------------

  // ---------------------- Memory Management Hooks -------------------
  ALWAYS_INLINE static void OnAllocatorMap(uptr p, uptr size) {}
  ALWAYS_INLINE static void OnAllocatorMapSecondary(uptr p, uptr size,
                                                    uptr user_begin,
                                                    uptr user_size) {}
  ALWAYS_INLINE static void OnAllocatorUnmap(uptr p, uptr size) {}
  ALWAYS_INLINE static void OnXsanAllocHook(uptr ptr, uptr size,
                                            BufferedStackTrace *stack) {}
  ALWAYS_INLINE static void OnXsanFreeHook(uptr ptr, uptr size,
                                           BufferedStackTrace *stack) {}
  ALWAYS_INLINE static void OnXsanAllocFreeTailHook(uptr pc) {}
  // ASan replaces allocs with fake stack frames, so we need to track them.
  // E.g., MSan needs to poison the fake stack frames.
  ALWAYS_INLINE static void OnFakeStackAlloc(uptr addr, uptr size) {}
  ALWAYS_INLINE static void OnFakeStackFree(uptr addr, uptr size) {}
  ALWAYS_INLINE static void OnFakeStackDestroy(uptr addr, uptr size) {}
  // 1. It must not process pending signals.
  //    Signal handlers may contain MOVDQA instruction (see below).
  // 2. It must be as simple as possible to not contain MOVDQA.
  ALWAYS_INLINE static void OnDtlsAlloc(uptr addr, uptr size) {}
  // ---------- End of Memory Management Hooks -------------------

  // ---------------------- pthread-related hooks -----------------
  /* pthread_create, pthread_join, pthread_detach, pthread_tryjoin_np, ... */
  class ScopedPthreadJoin {
   public:
    /// args:
    ///   - res : the return value of pthread_join, 0 for success, non-zero for
    ///   failure.
    ///   - ctx : the context of sub-sanitizer.
    ///   - th : the pthread_t of the thread to be joined.
    ALWAYS_INLINE ScopedPthreadJoin(const int &res, const Context &ctx,
                                    const void *th) {}
    ALWAYS_INLINE ~ScopedPthreadJoin() {}
  };
  class ScopedPthreadDetach {
   public:
    ALWAYS_INLINE ScopedPthreadDetach(const int &res, const Context &ctx,
                                      const void *th) {}
    ALWAYS_INLINE ~ScopedPthreadDetach() {}
  };
  class ScopedPthreadTryJoin {
   public:
    ALWAYS_INLINE ScopedPthreadTryJoin(const int &res, const Context &ctx,
                                       const void *th) {}
    ALWAYS_INLINE ~ScopedPthreadTryJoin() {}
  };
  ALWAYS_INLINE static void OnPthreadCreate() {}
  // ---------------- End of pthread-related hooks -----------------

  // ---------------------- Special Function Hooks -----------------
  ALWAYS_INLINE static void AtExit() {}
  class ScopedAtExitWrapper {
   public:
    ALWAYS_INLINE ScopedAtExitWrapper(uptr pc, const void *ctx) {}
    ALWAYS_INLINE ~ScopedAtExitWrapper() {}
  };
  class ScopedAtExitHandler {
   public:
    ALWAYS_INLINE ScopedAtExitHandler(uptr pc, const void *ctx) {}
    ALWAYS_INLINE ~ScopedAtExitHandler() {}
  };

  /*
  vfork interceptor (Refer to xsan_interceptors_vfork.S for details)
  - (Parent) vfork_before
  - (Parent/Child) vfork
  - (Child) vfork_child_after
  - The following hooks' execution order is not guaranteed
    - (Parent) vfork_parent_after
    - (Parent) vfork_parent_after_handle_sp
  */
  // Run before and after vfork, return a off-stack spill area to store the ra
  // for the vfork interceptor. Refer to xsan_interceptors_vfork.S for details.
  /// @return: the off-stack spill area.
  ALWAYS_INLINE static void vfork_before() {}
  ALWAYS_INLINE static void vfork_child_after() {}
  ALWAYS_INLINE static void vfork_parent_after() {}
  /// Only called in the parent process after vfork.
  /// @param sp: the stack pointer of the child process.
  ALWAYS_INLINE static void vfork_parent_after_handle_sp(void *sp) {}
  ALWAYS_INLINE static void OnForkBefore() {}
  ALWAYS_INLINE static void OnForkAfter(bool is_child) {}

  // Related to dlopen
  ALWAYS_INLINE static void BeforeDlopen(const char *filename, int flag) {}
  ALWAYS_INLINE static void OnLibraryLoaded(const char *filename,
                                            void *handle) {}
  // Related to dlclose
  ALWAYS_INLINE static void OnLibraryUnloaded() {}
  ALWAYS_INLINE static void OnLongjmp(void *env, const char *fn_name, uptr pc) {
  }

  // Unpoison the related shadow of the range.
  // Now only used for interceptor of swapcontext
  // ASan and MSan require to clear their shadow, while TSan does not.
  ALWAYS_INLINE static void ClearShadowMemoryForContextStack(void *addr,
                                                             uptr size) {}
  // --------- End of Special Function Hooks ---------

  // ---------- Unwind-related Hooks ----------------
  ALWAYS_INLINE static void OnEnterUnwind() {}
  ALWAYS_INLINE static void OnExitUnwind() {}
  // ---------- End of Unwind-related Hooks ----------------

  // ---------- Require Stack Trace Hooks ----------------
  ALWAYS_INLINE static bool RequireStackTraces() { return false; }
  ALWAYS_INLINE static int RequireStackTracesSize() { return -1; }
  // ---------- End of Require Stack Trace Hooks ----------------

  // ---------------------- Flags Registration Hooks ---------------
  ALWAYS_INLINE static void InitializeFlags() {}
  ALWAYS_INLINE static void SetCommonFlags(CommonFlags &cf) {}
  ALWAYS_INLINE static void ValidateFlags() {}
  // ----------End of Flags Registration Hooks ---------------

  // ---------- Thread-Related Hooks --------------------------
  ALWAYS_INLINE static void SetThreadName(const char *name) {}
  ALWAYS_INLINE static void SetThreadNameByUserId(uptr uid, const char *name) {}
  /*
  The following events happen in order while `pthread_create` is executed:

  - [parent-thread] : pthread_create
    - [parent-thread] : XsanThread::Create
      - [parent-thread] : (**HOOK**) XsanThread::CreateThread
    - [child-thread] : xsan_thread_start
      - [child-thread] : XsanThread::ThreadInit
        - [child-thread] : (**HOOK**) XsanThread::ChildThreadInit
      - [child-thread] : XsanThread::ThreadStart (with scoper)
        - [child-thread] : (**HOOK**) XsanThread::ChildThreadStart
        - [child-thread] : start_routine_

  - [child-thread] : TSD destroy / Active destroy
    - [child-thread] : XsanThread::Destroy
      - [child-thread] : (**HOOK**) XsanThread::DestroyThread
  */
  ALWAYS_INLINE static Thread CreateMainThread() { return Thread{}; }
  /// TODO: figure out data
  ALWAYS_INLINE static Thread CreateThread(u32 parent_tid, uptr child_uid,
                                           StackTrace *stack, const void *data,
                                           uptr data_size, bool detached) {
    return Thread{};
  }
  ALWAYS_INLINE static void ChildThreadInit(Thread &thread, tid_t os_id) {}
  ALWAYS_INLINE static void ChildThreadStart(Thread &thread, tid_t os_id) {}
  ALWAYS_INLINE static void DestroyThread(Thread &thread) {}
  // ---------- End of Thread-Related Hooks --------------------------

  // ---------- Synchronization and File-Related Hooks ------------------------
  ALWAYS_INLINE static void AfterMmap(const Context &ctx, void *res, uptr size,
                                      int fd) {}
  ALWAYS_INLINE static void BeforeMunmap(const Context &ctx, void *addr,
                                         uptr size) {}
  ALWAYS_INLINE static void FdAccess(const Context &ctx, int fd) {}
  ALWAYS_INLINE static void FdPipeCreate(const Context &ctx, int fd0, int fd1) {
  }
  ALWAYS_INLINE static void FdAcquire(const Context &ctx, int fd) {}
  ALWAYS_INLINE static void BeforeDlIteratePhdrCallback(
      const Context &ctx, __sanitizer_dl_phdr_info &info, SIZE_T size) {}
  ALWAYS_INLINE static void AfterDlIteratePhdrCallback(
      const Context &ctx, __sanitizer_dl_phdr_info &info, SIZE_T size) {}
  // ---------- End of Synchronization and File-Related Hooks ----------------

  // ---------- Func to use special scope ------------------------
  template <ScopedFunc func>
  struct FuncScope {};
  // ---------- End of Func to use special scope ----------------

  // ---------- Generic Hooks in Interceptors ----------------
  ALWAYS_INLINE static void InitializeInterceptors() {}
  PSEUDO_MACRO static void ReadRange(const Context *ctx, const void *offset,
                                     uptr size, const char *func_name) {}
  PSEUDO_MACRO static void WriteRange(const Context *ctx, const void *offset,
                                      uptr size, const char *func_name) {}
  // "use" means that the value is:
  // 1. dereferenced as a pointer
  // 2. used for conditional judgement
  // 3. used for system call
  // 4. loaded to a floating point register
  /// TODO: whether 'ctx' is needed, some places use nullptr now:
  // 1. 'sigaction_impl' in 'tsan_interceptors.cpp'
  PSEUDO_MACRO static void UseRange(const Context *ctx, const void *offset,
                                    uptr size, const char *func_name) {}
  // Copy hook emphasizes the behavior of copy. Read/Write hooks must be around
  // it. If we just care about the write/read semantic, implement the write/read
  // hooks.
  PSEUDO_MACRO static void CopyRange(const Context *ctx, const void *dst,
                                     const void *src, uptr size,
                                     BufferedStackTrace &stack) {}
  // "move" still works when source and destination overlap, like memmove.
  PSEUDO_MACRO static void MoveRange(const Context *ctx, const void *dst,
                                     const void *src, uptr size,
                                     BufferedStackTrace &stack) {}
  // Some function manipulate two ranges and not allowed to overlap, e.g.,
  // strcat, strncat, etc.
  // ASan can use this hook to check if the two ranges overlap.
  PSEUDO_MACRO static void OnTwoRangesOverlap(const char *offset1, uptr size1,
                                              const char *offset2, uptr size2,
                                              const char *func_name) {}
  /// TODO: whether 'ctx' is needed, some places use nullptr now:
  // 1. 'mallinfo' in 'xsan_malloc_linux.cpp'
  // 2. 'XSAN_COMMON_INIT_RANGE' in 'xsan_interceptors_memintrinsics.h'
  // 3. 'sigaction_impl' in 'tsan_interceptors.cpp'
  PSEUDO_MACRO static void InitRange(const Context *ctx, const void *offset,
                                     uptr size) {}
  PSEUDO_MACRO static void CommonReadRange(const Context *ctx,
                                           const void *offset, uptr size,
                                           const char *func_name) {}
  PSEUDO_MACRO static void CommonWriteRange(const Context *ctx,
                                            const void *offset, uptr size,
                                            const char *func_name) {}
  PSEUDO_MACRO static void CommonUnpoisonParam(uptr count) {}
  PSEUDO_MACRO static void CommonInitRange(const Context *ctx,
                                           const void *offset, uptr size) {}
  PSEUDO_MACRO static void CommonSyscallPreReadRange(const Context &ctx,
                                                     const void *offset,
                                                     uptr size,
                                                     const char *func_name) {}
  PSEUDO_MACRO static void CommonSyscallPostReadRange(const Context &ctx,
                                                      const void *offset,
                                                      uptr size,
                                                      const char *func_name) {}
  PSEUDO_MACRO static void CommonSyscallPreWriteRange(const Context &ctx,
                                                      const void *offset,
                                                      uptr size,
                                                      const char *func_name) {}
  PSEUDO_MACRO static void CommonSyscallPostWriteRange(const Context &ctx,
                                                       const void *offset,
                                                       uptr size,
                                                       const char *func_name) {}
  // ---------- End of Generic Hooks in Interceptors ----------------

  // ---------- Xsan-Interface-Related Hooks ----------------
  template <u32 ReadSize>
  ALWAYS_INLINE static void __xsan_unaligned_read(uptr p) {}
  template <u32 WriteSize>
  ALWAYS_INLINE static void __xsan_unaligned_write(uptr p) {}
  template <u32 ReadSize>
  ALWAYS_INLINE static void __xsan_read(uptr p) {}
  template <u32 WriteSize>
  ALWAYS_INLINE static void __xsan_write(uptr p) {}
  // ---------- End of Xsan-Interface-Related Hooks ----------------
};

}  // namespace __xsan
