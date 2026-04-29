//===-- xsan_hooks.cpp ---------------------------------------------------===//
//
// This file is a part of XSan, an composition of several sanitizers.
//
// This file is used to manuplate the hooks for coordination between multiple
// sanitizers.
//===----------------------------------------------------------------------===//

#include "xsan_hooks.h"

#include "asan/asan_thread.h"
#include "asan/orig/asan_internal.h"
#include "lsan/lsan_common.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "xsan_hooks_dispatch.h"
#include "xsan_interceptors.h"
#include "xsan_interface_internal.h"
#include "xsan_internal.h"
#include "xsan_thread.h"

#if XSAN_CONTAINS_TSAN
#  include "tsan/tsan_rtl.h"
#endif

using namespace __xsan;
// ---------------------- State/Ignoration Management Hooks --------------------
namespace __xsan {
THREADLOCAL int xsan_in_internal = 0;
THREADLOCAL int is_in_symbolizer = 0;
THREADLOCAL int xsan_in_unwind = 0;

int get_exit_code(const void *ctx) {
  int exit_code = 0;
  if (CAN_SANITIZE_LEAKS && common_flags()->detect_leaks &&
      __lsan::HasReportedLeaks()) {
    return common_flags()->exitcode;
  }
#if XSAN_CONTAINS_TSAN
  auto *tsan_thr =
      ctx == nullptr
          ? __tsan::cur_thread()
          : ((const XsanInterceptorContext *)ctx)->xsan_ctx.tsan.thr_;
  exit_code = __tsan::Finalize(tsan_thr);
#endif
  return exit_code;
}

}  // namespace __xsan

// ---------- End of State Management Hooks -----------------

// ---------------------- Memory Management Hooks -------------------
/// As XSan uses ASan's heap allocator and fake stack directly, hence we don't
/// need to invoke ASan's hooks here.

namespace __asan {

SANITIZER_WEAK_ATTRIBUTE
bool GetASanMellocStackTrace(u32 &stack_trace_id, uptr addr,
                             bool set_stack_trace_id) {
  return false;
}

}  // namespace __asan

namespace __xsan {

bool GetMellocStackTrace(u32 &stack_trace_id, uptr addr,
                         bool set_stack_trace_id) {
  return __asan::GetASanMellocStackTrace(stack_trace_id, addr,
                                         set_stack_trace_id);
}

}  // namespace __xsan

// ---------- End of Memory Management Hooks -------------------

// ---------------------- Special Function Hooks -----------------
namespace __xsan {
/*
Provides hooks for special functions, such as
  - atexit / on_exit / _cxa_atexit
  - longjmp / siglongjmp / _longjmp / _siglongjmp
  - dlopen / dlclose
  - vfork / fork
*/

/// To implement macro COMMON_INTERCEPTOR_SPILL_AREA in *vfork.S
/// Notably, this function is called TWICE at the attitude per process.
extern "C" void *__xsan_extra_spill_area() {
  // No process has ID -1.
  static THREADLOCAL uptr pid_before_vfork = -1;
  uptr pid = internal_getpid();
  uptr ppid = internal_getppid();
  // If pid != pid_before_vfork, it must be the first time to call
  // vfork_before_and_after in the process.
  if (pid != pid_before_vfork) {
    // If ppid == pid_before_vfork, it must be the child process calling
    // vfork_before_and_after.
    if (ppid == pid_before_vfork) {
      XSAN_HOOKS_EXEC(vfork_child_after);
    }
    // Otherwise, it must be the parent process calling vfork_before_and_after.
    else {
      // ----------  [Parent] before vfork ----------------
      pid_before_vfork = pid;
      XSAN_HOOKS_EXEC(vfork_before);
    }
  }
  // Otherwise, it must be the parent process calling vfork_before_and_after
  // for the second time.
  else {
    // ------------  [Parent] after vfork ----------------
    // Must after '[Child] after vfork', as parent process will suspend until
    // child process `exit`/`exec`.
    // Reset pid_before_vfork to -1 to recover the state of this hook.
    // I.e., make vfork -> fork -> [child] vfork works.
    pid_before_vfork = -1;
    XSAN_HOOKS_EXEC(vfork_parent_after);
  }
  XsanThread *t = GetCurrentThread();
  CHECK(t);
  return t->extra_spill_area();
}

extern "C" void __xsan_vfork_parent_after(void *sp) {
  /// Unpoison vfork child's new stack space : [stack_bottom, sp]
  XSAN_HOOKS_EXEC(vfork_parent_after_handle_sp, sp);
}
// Ignore interceptors in OnLibraryLoaded()/Unloaded().  These hooks use code
// (ListOfModules::init, MemoryMappingLayout::DumpListOfModules) that make
// intercepted calls, which can cause deadlockes with ReportRace() which also
// uses this code.
void OnLibraryLoaded(const char *filename, void *handle) {
  __xsan::ScopedIgnoreInterceptors ignore;
  XSAN_HOOKS_EXEC(OnLibraryLoaded, filename, handle);
}

void OnLibraryUnloaded() {
  __xsan::ScopedIgnoreInterceptors ignore;
  XSAN_HOOKS_EXEC(OnLibraryUnloaded);
}
}  // namespace __xsan

// --------------- End of Special Function Hooks -----------------

// --------------- Hooks of Report ---------------------------------------
/*
  Each sub-sanitizer defines its own Bug Report function, and it seems that
  XSan cannot provide a hook for Bug Reports without modifying these functions.
  Fortunately, almost all Sanitizer Bug Reports are protected by
  __sanitizer::ScopedErrorReportLock, so we can intercept Lock/Unlock at link
  time to expose hooks before/after report:
    - __sanitizer::ScopedErrorReportLock::Lock()   : provides EnterReport() hook
    - __sanitizer::ScopedErrorReportLock::Unlock() : provides ExitReport() hook
    - __sanitizer::StackTrace::Print() : might call Symbolizer, intercept it
*/

/// __sanitizer::ScopedErrorReportLock::Lock()
XSAN_WRAPPER(void, _ZN11__sanitizer21ScopedErrorReportLock4LockEv, ) {
  XSAN_REAL(_ZN11__sanitizer21ScopedErrorReportLock4LockEv)();
  XSAN_HOOKS_EXEC(EnterReport);
}

/// __sanitizer::ScopedErrorReportLock::Unlock()
XSAN_WRAPPER(void, _ZN11__sanitizer21ScopedErrorReportLock6UnlockEv, ) {
  XSAN_HOOKS_EXEC(ExitReport);
  XSAN_REAL(_ZN11__sanitizer21ScopedErrorReportLock6UnlockEv)();
}

/// __sanitizer::StackTrace::Print() const
XSAN_WRAPPER(void, _ZNK11__sanitizer10StackTrace5PrintEv,
             const StackTrace *self) {
  __xsan::ScopedXsanInternal scope;
  if (!GetCurrentThread()) {
    // If we cannot obtain the current thread, we are in the thread created by
    // internal_start_thread, i.e., internal thread.
    XSAN_REAL(_ZNK11__sanitizer10StackTrace5PrintEv)(self);
  } else {
    // ScopedIgnoreInterceptors is used to ignore interceptors for user-threads.
    __xsan::ScopedIgnoreInterceptors ignore(true);
    XSAN_REAL(_ZNK11__sanitizer10StackTrace5PrintEv)(self);
  }
}
