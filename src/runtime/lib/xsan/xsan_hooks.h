//===-- xsan_hooks.h ------------------------------------------*- C++
//-*-===//
//
// This file is a part of XSan, an composition of several sanitizers.
//
// This file is to offer integrated hooks.
//===----------------------------------------------------------------------===//
#pragma once

#include <sanitizer_common/sanitizer_common.h>
#include <sanitizer_common/sanitizer_internal_defs.h>
#include <sanitizer_common/sanitizer_platform_limits_posix.h>

#include "xsan_attribute.h"
#include "xsan_hooks_dispatch.h"
#include "xsan_interface_internal.h"

namespace __sanitizer {
struct CommonFlags;
}

namespace __xsan {

// These structs is to hold the context of sub-sanitizers.
// - TSan requires a thread state and a pc everywhere.
// - ASan needs a BufferedStackTrace everywhere.
struct XsanContext {
  XSAN_HOOKS_DEFINE_VAR(Context)
  XSAN_HOOKS_DEFINE_VAR_CVT(Context)
  XSAN_HOOKS_DEFINE_PTR_VAR(XsanContext, Context)

  XsanContext() : XSAN_HOOKS_INIT_VAR() {}
  XsanContext(uptr pc) : XSAN_HOOKS_INIT_VAR(pc) {}
};

struct XsanInterceptorContext {
  const char *interceptor_name;
  /// TODO: should use pointer or reference?
  XsanContext &xsan_ctx;
};

// ---------------------- Hook for other Sanitizers -------------------
ALWAYS_INLINE void InitFromXsanVeryEarly() {
  XSAN_HOOKS_EXEC(InitFromXsanVeryEarly);
}
ALWAYS_INLINE void InitFromXsanEarly() { XSAN_HOOKS_EXEC(InitFromXsanEarly); }
ALWAYS_INLINE void InitFromXsan() { XSAN_HOOKS_EXEC(InitFromXsan); }
ALWAYS_INLINE void InitFromXsanLate() { XSAN_HOOKS_EXEC(InitFromXsanLate); }

template <typename Container>
ALWAYS_INLINE void NeededMapRanges(Container &res) {
  XSAN_HOOKS_EXEC_EXTEND(res, NeededMapRanges);
}

extern THREADLOCAL int is_in_symbolizer;
ALWAYS_INLINE bool in_symbolizer() { return UNLIKELY(is_in_symbolizer > 0); }

ALWAYS_INLINE void EnterSymbolizer() {
  ++is_in_symbolizer;
  XSAN_HOOKS_EXEC(EnterSymbolizer);
}

ALWAYS_INLINE void ExitSymbolizer() {
  --is_in_symbolizer;
  XSAN_HOOKS_EXEC(ExitSymbolizer);
}

extern THREADLOCAL int xsan_in_unwind;
ALWAYS_INLINE void OnEnterUnwind() {
  ++xsan_in_unwind;
  XSAN_HOOKS_EXEC(OnEnterUnwind);
}
ALWAYS_INLINE void OnExitUnwind() {
  --xsan_in_unwind;
  XSAN_HOOKS_EXEC(OnExitUnwind);
}
ALWAYS_INLINE bool IsInUnwind() { return UNLIKELY(xsan_in_unwind > 0); }

// If in symbolizer or unwinding, we are reporting errors, should not do
// sanity checks and ignore interceptors.
ALWAYS_INLINE bool IsInSymbolizerOrUnwind() {
  return in_symbolizer() || IsInUnwind();
}

/// Notifies Xsan that the current thread is entering an completely internal
/// Xsan function, e.g., __asan_handle_no_return.
/// In such cases, Xsan should not do sanity checks.
/// Compared to ShouldIgnoreInterceptos(), this function ignores in_ignore_lib.
extern THREADLOCAL int xsan_in_internal;
ALWAYS_INLINE bool IsInXsanInternal() {
  return xsan_in_internal != 0 ||
         /* If in symbolizer or unwind, we are also in Xsan internal */
         IsInSymbolizerOrUnwind();
}
class ScopedXsanInternal {
 public:
  ALWAYS_INLINE ScopedXsanInternal() { xsan_in_internal++; }
  ALWAYS_INLINE ~ScopedXsanInternal() { xsan_in_internal--; }
};

ALWAYS_INLINE bool ShouldSanitzerIgnoreInterceptors(
    const XsanContext &xsan_thr) {
  /// Avoid sanity checks in XSan internal or SymbolizerOrUnwind
  if (IsInXsanInternal())
    return true;
  bool should_ignore = false;
  /// TODO: to support libignore, we plan to migrate it to Xsan.
  /// xsan_suppressions.cpp is required accordingly.
  XSAN_HOOKS_EXEC_OR(should_ignore, ShouldIgnoreInterceptors, xsan_thr);
  return should_ignore;
}

ALWAYS_INLINE bool ShouldSanitzerIgnoreAllocFreeHook() {
  bool should_ignore = false;
  XSAN_HOOKS_EXEC_OR(should_ignore, ShouldIgnoreAllocFreeHook);
  return should_ignore;
}
// Integrates different sanitizers' exit code logic.
/// TODO: unify the exit code logic.
int get_exit_code(const void *ctx = nullptr);

ALWAYS_INLINE void OnAllocatorMap(uptr p, uptr size) {
  XSAN_HOOKS_EXEC(OnAllocatorMap, p, size);
}

ALWAYS_INLINE void OnAllocatorMapSecondary(uptr p, uptr size, uptr user_begin,
                                           uptr user_size) {
  XSAN_HOOKS_EXEC(OnAllocatorMapSecondary, p, size, user_begin, user_size);
}

ALWAYS_INLINE void OnAllocatorUnmap(uptr p, uptr size) {
  XSAN_HOOKS_EXEC(OnAllocatorUnmap, p, size);
}

ALWAYS_INLINE void XsanAllocHook(uptr ptr, uptr size,
                                 BufferedStackTrace *stack) {
  XSAN_HOOKS_EXEC(OnXsanAllocHook, ptr, size, stack);
}

ALWAYS_INLINE void XsanFreeHook(uptr ptr, uptr size,
                                BufferedStackTrace *stack) {
  XSAN_HOOKS_EXEC(OnXsanFreeHook, ptr, size, stack);
}

ALWAYS_INLINE void XsanAllocFreeTailHook(uptr pc) {
  XSAN_HOOKS_EXEC(OnXsanAllocFreeTailHook, pc);
}

ALWAYS_INLINE void OnFakeStackAlloc(uptr addr, uptr size) {
  XSAN_HOOKS_EXEC(OnFakeStackAlloc, addr, size);
}

ALWAYS_INLINE void OnFakeStackFree(uptr addr, uptr size) {
  XSAN_HOOKS_EXEC(OnFakeStackFree, addr, size);
}

ALWAYS_INLINE void OnFakeStackDestroy(uptr addr, uptr size) {
  XSAN_HOOKS_EXEC(OnFakeStackDestroy, addr, size);
}

ALWAYS_INLINE void OnDtlsAlloc(uptr addr, uptr size) {
  XSAN_HOOKS_EXEC(OnDtlsAlloc, addr, size);
}

class ScopedPthreadJoin {
 public:
  /// args:
  ///   - res : the return value of pthread_join, 0 for success, non-zero for
  ///   failure.
  ///   - xsan_ctx : the context of Xsan.
  ///   - th : the pthread_t of the thread to be joined.
  ScopedPthreadJoin(const int &res, const __xsan::XsanContext &xsan_ctx,
                    const void *th)
      : XSAN_HOOKS_INIT_VAR(res, xsan_ctx, th) {}
  ~ScopedPthreadJoin() {}

 private:
  XSAN_HOOKS_DEFINE_VAR(ScopedPthreadJoin)
};

class ScopedPthreadDetach {
 public:
  ScopedPthreadDetach(const int &res, const __xsan::XsanContext &xsan_ctx,
                      const void *th)
      : XSAN_HOOKS_INIT_VAR(res, xsan_ctx, th) {}
  ~ScopedPthreadDetach() {}

 private:
  XSAN_HOOKS_DEFINE_VAR(ScopedPthreadDetach)
};

class ScopedPthreadTryJoin {
 public:
  ScopedPthreadTryJoin(const int &res, const __xsan::XsanContext &xsan_ctx,
                       const void *th)
      : XSAN_HOOKS_INIT_VAR(res, xsan_ctx, th) {}
  ~ScopedPthreadTryJoin() {}

 private:
  XSAN_HOOKS_DEFINE_VAR(ScopedPthreadTryJoin)
};

// ---------------------- Special Function Hooks -----------------

ALWAYS_INLINE void AtExit() { XSAN_HOOKS_EXEC(AtExit); }

class ScopedAtExitWrapper {
 public:
  ScopedAtExitWrapper(uptr pc, const void *ctx)
      : XSAN_HOOKS_INIT_VAR(pc, ctx) {}
  ~ScopedAtExitWrapper() {}

 private:
  XSAN_HOOKS_DEFINE_VAR(ScopedAtExitWrapper)
};
class ScopedAtExitHandler {
 public:
  ScopedAtExitHandler(uptr pc, const void *ctx)
      : XSAN_HOOKS_INIT_VAR(pc, ctx) {}
  ~ScopedAtExitHandler() {}

 private:
  XSAN_HOOKS_DEFINE_VAR(ScopedAtExitHandler)
};

ALWAYS_INLINE void OnPthreadCreate() { XSAN_HOOKS_EXEC(OnPthreadCreate); }

ALWAYS_INLINE void InitializeSanitizerFlags() {
  XSAN_HOOKS_EXEC(InitializeFlags);
}
ALWAYS_INLINE void SetSanitizerCommonFlags(CommonFlags &cf) {
  XSAN_HOOKS_EXEC(SetCommonFlags, cf);
}
ALWAYS_INLINE void ValidateSanitizerFlags() { XSAN_HOOKS_EXEC(ValidateFlags); }

ALWAYS_INLINE void SetSanitizerThreadName(const char *name) {
  XSAN_HOOKS_EXEC(SetThreadName, name);
}
ALWAYS_INLINE void SetSanitizerThreadNameByUserId(uptr uid, const char *name) {
  XSAN_HOOKS_EXEC(SetThreadNameByUserId, uid, name);
}

bool GetMellocStackTrace(u32 &stack_trace_id, uptr addr,
                         bool set_stack_trace_id);

ALWAYS_INLINE void AfterMmap(const XsanInterceptorContext &ctx, void *res,
                             uptr size, int fd) {
  XSAN_HOOKS_EXEC(AfterMmap, ctx.xsan_ctx, res, size, fd);
}
ALWAYS_INLINE void BeforeMunmap(const XsanInterceptorContext &ctx, void *addr,
                                uptr size) {
  /// Size too big can cause problems with the shadow memory.
  /// unmap on NULL is not allowed.
  if ((sptr)size < 0 || addr == nullptr)
    return;
  XSAN_HOOKS_EXEC(BeforeMunmap, ctx.xsan_ctx, addr, size);
}
ALWAYS_INLINE void FdAccess(const XsanInterceptorContext &ctx, int fd) {
  XSAN_HOOKS_EXEC(FdAccess, ctx.xsan_ctx, fd);
}
ALWAYS_INLINE void FdPipeCreate(const XsanInterceptorContext &ctx, int fd0,
                                int fd1) {
  XSAN_HOOKS_EXEC(FdPipeCreate, ctx.xsan_ctx, fd0, fd1);
}
ALWAYS_INLINE void FdAcquire(const XsanInterceptorContext &ctx, int fd) {
  XSAN_HOOKS_EXEC(FdAcquire, ctx.xsan_ctx, fd);
}
ALWAYS_INLINE void BeforeDlIteratePhdrCallback(
    const XsanInterceptorContext &ctx, __sanitizer_dl_phdr_info &info,
    SIZE_T size) {
  XSAN_HOOKS_EXEC(BeforeDlIteratePhdrCallback, ctx.xsan_ctx, info, size);
}
ALWAYS_INLINE void AfterDlIteratePhdrCallback(const XsanInterceptorContext &ctx,
                                              __sanitizer_dl_phdr_info &info,
                                              SIZE_T size) {
  XSAN_HOOKS_EXEC(AfterDlIteratePhdrCallback, ctx.xsan_ctx, info, size);
}
/// To implement macro COMMON_INTERCEPTOR_HANDLE_VFORK in *vfork.S
extern "C" void __xsan_vfork_parent_after(void *sp);
/// Used to lock before fork
ALWAYS_INLINE void OnForkBefore() { XSAN_HOOKS_EXEC(OnForkBefore); }
/// Used to unlock after fork
ALWAYS_INLINE void OnForkAfter(bool is_child) {
  XSAN_HOOKS_EXEC(OnForkAfter, is_child);
}
ALWAYS_INLINE void BeforeDlopen(const char *filename, int flag) {
  XSAN_HOOKS_EXEC(BeforeDlopen, filename, flag);
}
void OnLibraryLoaded(const char *filename, void *handle);
void OnLibraryUnloaded();
ALWAYS_INLINE void OnLongjmp(void *env, const char *fn_name, uptr pc) {
  XSAN_HOOKS_EXEC(OnLongjmp, env, fn_name, pc);
}
ALWAYS_INLINE void ClearShadowMemoryForContextStack(void *addr, uptr size) {
  XSAN_HOOKS_EXEC(ClearShadowMemoryForContextStack, addr, size);
}

enum class XsanStackTraceType { copy };

template <XsanStackTraceType type>
ALWAYS_INLINE bool RequireStackTraces() {
  bool require = false;
  XSAN_HOOKS_EXEC_OR(require, RequireStackTraces);
  return require;
}

template <XsanStackTraceType type>
ALWAYS_INLINE int RequireStackTracesSize() {
  int size = -1;
  XSAN_HOOKS_EXEC_MAX(size, RequireStackTracesSize);
  return size;
}

template <ScopedFunc func>
struct XsanFuncScope {
  XSAN_HOOKS_DEFINE_VAR(FuncScope<func>)

  // To eliminate the stack frame when reporting error in the constructor.
  PSEUDO_MACRO XsanFuncScope() {}
  PSEUDO_MACRO ~XsanFuncScope() {}
};

ALWAYS_INLINE void InitializeInterceptors() {
  XSAN_HOOKS_EXEC(InitializeInterceptors);
}
PSEUDO_MACRO void ReadRange(void *_ctx, const void *offset, uptr size) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(ReadRange, XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr},
                  offset, size, (ctx ? ctx->interceptor_name : nullptr));
}
PSEUDO_MACRO void WriteRange(void *_ctx, const void *offset, uptr size) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(WriteRange, XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr},
                  offset, size, (ctx ? ctx->interceptor_name : nullptr));
}
// "use" means that the value is:
// 1. dereferenced as a pointer
// 2. used for conditional judgement
// 3. used for system call
// 4. loaded to a floating point register
/// TODO: whether 'ctx' is needed, some places use nullptr now:
// 1. 'sigaction_impl' in 'tsan_interceptors.cpp'
// 2. 'CallUserSignalHandler' in 'tsan_interceptors.cpp'
PSEUDO_MACRO void UseRange(void *_ctx, const void *offset, uptr size) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(UseRange, XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr},
                  offset, size, (ctx ? ctx->interceptor_name : __func__));
}
PSEUDO_MACRO void CopyRange(void *_ctx, const void *dst, const void *src,
                            uptr size, BufferedStackTrace &stack) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(CopyRange, XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr},
                  dst, src, size, stack);
}
// "move" works properly when source and destination overlap, like memmove.
PSEUDO_MACRO void MoveRange(void *_ctx, const void *dst, const void *src,
                            uptr size, BufferedStackTrace &stack) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(MoveRange, XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr},
                  dst, src, size, stack);
}
// Hook on function call related to TWO ranges.
// ASan can use this hook to check if the two ranges overlap.
PSEUDO_MACRO void OnTwoRangesOverlap(const char *offset1, uptr size1,
                                     const char *offset2, uptr size2,
                                     const char *func_name) {
  XSAN_HOOKS_EXEC(OnTwoRangesOverlap, offset1, size1, offset2, size2,
                  func_name);
}

/// TODO: whether 'ctx' is needed, some places use nullptr now:
// 1. 'mallinfo' in 'xsan_malloc_linux.cpp'
// 2. 'XSAN_COMMON_INIT_RANGE' in 'xsan_interceptors_memintrinsics.h'
// 3. 'sigaction_impl' in 'tsan_interceptors.cpp'
// 4. 'CallUserSignalHandler' in 'tsan_interceptors.cpp'
PSEUDO_MACRO void InitRange(void *_ctx, const void *offset, uptr size) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(InitRange, XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr},
                  offset, size);
}
PSEUDO_MACRO void CommonReadRange(void *_ctx, const void *offset, uptr size) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(CommonReadRange,
                  XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr}, offset,
                  size, (ctx ? ctx->interceptor_name : nullptr));
}
PSEUDO_MACRO void CommonWriteRange(void *_ctx, const void *offset, uptr size) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(CommonWriteRange,
                  XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr}, offset,
                  size, (ctx ? ctx->interceptor_name : nullptr));
}
PSEUDO_MACRO void CommonUnpoisonParam(uptr count) {
  XSAN_HOOKS_EXEC(CommonUnpoisonParam, count);
}
PSEUDO_MACRO void CommonInitRange(void *_ctx, const void *offset, uptr size) {
  XsanInterceptorContext *ctx = (XsanInterceptorContext *)_ctx;
  XSAN_HOOKS_EXEC(CommonInitRange,
                  XsanContext::Ptr{ctx ? &ctx->xsan_ctx : nullptr}, offset,
                  size);
}
PSEUDO_MACRO void CommonSyscallPreReadRange(const XsanInterceptorContext &ctx,
                                            const void *offset, uptr size) {
  XSAN_HOOKS_EXEC(CommonSyscallPreReadRange, ctx.xsan_ctx, offset, size,
                  ctx.interceptor_name);
}
PSEUDO_MACRO void CommonSyscallPostReadRange(const XsanInterceptorContext &ctx,
                                             const void *offset, uptr size) {
  XSAN_HOOKS_EXEC(CommonSyscallPostReadRange, ctx.xsan_ctx, offset, size,
                  ctx.interceptor_name);
}
PSEUDO_MACRO void CommonSyscallPreWriteRange(const XsanInterceptorContext &ctx,
                                             const void *offset, uptr size) {
  XSAN_HOOKS_EXEC(CommonSyscallPreWriteRange, ctx.xsan_ctx, offset, size,
                  ctx.interceptor_name);
}
PSEUDO_MACRO void CommonSyscallPostWriteRange(const XsanInterceptorContext &ctx,
                                              const void *offset, uptr size) {
  XSAN_HOOKS_EXEC(CommonSyscallPostWriteRange, ctx.xsan_ctx, offset, size,
                  ctx.interceptor_name);
}

}  // namespace __xsan
