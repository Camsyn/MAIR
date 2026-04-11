#pragma once

/// Directly copied from somewhere of TSan to provide some interfaces to hooks.

#include "../xsan_platform.h"
#include "sanitizer_common/sanitizer_flags.h"
#include "sanitizer_common/sanitizer_internal_defs.h"

namespace __tsan {

void EnterSymbolizer();
void ExitSymbolizer();

struct alignas(SANITIZER_CACHE_LINE_SIZE) ThreadState;

ThreadState *cur_thread();
ThreadState *cur_thread_init();

// This creates 2 non-inlined specialized versions of MemoryAccessRange.
template <bool is_read>
void MemoryAccessRangeT(ThreadState *thr, uptr pc, uptr addr, uptr size);

void InitializeFlags();
void InitializeInterceptors();
// Initialization before flag initialization
void TsanInitFromXsanVeryEarly();
void InitializePlatformEarly();
// Initialization before xsan_is_running = false;
void TsanInitFromXsan();
// Initialization after TSan has been fully initialized.
void TsanInitFromXsanLate();

void SetCommonFlags(CommonFlags &cf);
void ValidateFlags();
void SetTsanThreadName(const char *name);
void SetTsanThreadNameByUserId(uptr uid, const char *name);
void DisableTsanForVfork();
void RecoverTsanAfterVforkParent();
void atfork_prepare();
void atfork_parent();
void atfork_child();

void ThreadIgnoreBegin(ThreadState *thr, uptr pc);
void ThreadIgnoreEnd(ThreadState *thr);

// Related to fd access
void FdAccess(ThreadState *thr, uptr pc, int fd);
void FdPipeCreate(ThreadState *thr, uptr pc, int rfd, int wfd);
void FdAcquire(ThreadState *thr, uptr pc, int fd);

}  // namespace __tsan
