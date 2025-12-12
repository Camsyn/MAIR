//===-- xsan_interface.cpp ------------------------------------------------===//
//
// This file is a part of XSan, a composition of different Sanitizers.
//
// This file is used to provide some uniform interface for XSan runtime.
//===----------------------------------------------------------------------===//

#include <sanitizer_common/sanitizer_atomic.h>
#include <sanitizer_common/sanitizer_flags.h>
#include <sanitizer_common/sanitizer_interface_internal.h>
#include <sanitizer_common/sanitizer_libc.h>
#include <sanitizer_common/sanitizer_ptrauth.h>

#include "xsan_interceptors.h"

using namespace __xsan;

/// TODO: use a more generic way to perform checks.
/// For example, provide some hooks for events, and integrate the checks into
/// these hooks.
///   - OnAllocation:
///   - OnFree:
///   - OnMemoryAccess:
///     ....

extern "C" {

#define UNALIGNED_LOAD(size)                                       \
  SANITIZER_INTERFACE_ATTRIBUTE                                    \
  u##size __sanitizer_unaligned_load##size(const uu##size *addr) { \
    XSAN_HOOKS_EXEC(__xsan_unaligned_read<size / 8>, (uptr)addr);  \
    return *addr;                                                  \
  }

UNALIGNED_LOAD(16)
UNALIGNED_LOAD(32)
UNALIGNED_LOAD(64)

#define UNALIGNED_STORE(size)                                         \
  SANITIZER_INTERFACE_ATTRIBUTE                                       \
  void __sanitizer_unaligned_store##size(uu##size *addr, u##size v) { \
    XSAN_HOOKS_EXEC(__xsan_unaligned_write<size / 8>, (uptr)addr);    \
    *addr = v;                                                        \
  }

UNALIGNED_STORE(16)
UNALIGNED_STORE(32)
UNALIGNED_STORE(64)

#undef UNALIGNED_LOAD
#undef UNALIGNED_STORE

SANITIZER_INTERFACE_ATTRIBUTE
void __xsan_read_range(const void *beg, const void *end, uptr pc = 0) {
  if (UNLIKELY(beg == end))
    return;
  /// The caller of __xsan_read_range should ensure that beg <= end
  DCHECK(beg < end && "Invalid range");
  uptr size = (uptr)end - (uptr)beg;
  // Printf("beg: %p, end: %p, size: %lx\n", beg, end, size);
  // set global pc to the scoped_pc one
  ScopedRealPc scoped_pc(pc);
  /// TODO: use a more specific function to perform the check.
  XSAN_READ_RANGE((void *)nullptr, beg, size);
}

SANITIZER_INTERFACE_ATTRIBUTE
void __xsan_write_range(const void *beg, const void *end, uptr pc = 0) {
  if (UNLIKELY(beg == end))
    return;
  /// The caller of __xsan_write_range should ensure that beg <= end
  DCHECK(beg < end && "Invalid range");
  uptr size = (uptr)end - (uptr)beg;
  // set global pc to the scoped_pc one
  ScopedRealPc scoped_pc(pc);
  XSAN_WRITE_RANGE((void *)nullptr, beg, size);
}

/// TODO: use SIMD to perform the check.
#define XSAN_PERIODICAL_OPERATION_CALLBACK_IMPL(operation, size_param)         \
  SANITIZER_INTERFACE_ATTRIBUTE                                                \
  void __xsan_period_##operation##size_param(const void *beg, const void *end, \
                                             s64 step, uptr pc = 0) {          \
    if (UNLIKELY(beg == end))                                                  \
      return;                                                                  \
    ScopedRealPc scoped_pc(pc);                                                \
    DCHECK(Abs(step) >= (size_param) && "Invalid arguments");                  \
    uptr L = (uptr)beg, R = (uptr)end;                                         \
    if (UNLIKELY((step) < 0)) {                                                \
      Swap(L, R);                                                              \
      L -= step;                                                               \
      R -= step;                                                               \
      __xsan_period_##operation##size_param((const void *)L, (const void *)R,  \
                                            -(step), pc);                      \
      return;                                                                  \
    }                                                                          \
    if (UNLIKELY(step == (size_param))) {                                      \
      __xsan_##operation##_range((const void *)L, (const void *)R, pc);        \
      return;                                                                  \
    }                                                                          \
    DCHECK(L <= R && "Invalid arguments");                                     \
    for (uptr offset = L; offset < R; offset += step) {                        \
      XSAN_HOOKS_EXEC(__xsan_##operation<size_param>, offset);                 \
    }                                                                          \
  }

#define XSAN_PERIODICAL_READ_CALLBACK(size) \
  XSAN_PERIODICAL_OPERATION_CALLBACK_IMPL(read, size)

#define XSAN_PERIODICAL_WRITE_CALLBACK(size) \
  XSAN_PERIODICAL_OPERATION_CALLBACK_IMPL(write, size)

XSAN_PERIODICAL_READ_CALLBACK(1)
XSAN_PERIODICAL_READ_CALLBACK(2)
XSAN_PERIODICAL_READ_CALLBACK(4)
XSAN_PERIODICAL_READ_CALLBACK(8)
XSAN_PERIODICAL_READ_CALLBACK(16)
XSAN_PERIODICAL_WRITE_CALLBACK(1)
XSAN_PERIODICAL_WRITE_CALLBACK(2)
XSAN_PERIODICAL_WRITE_CALLBACK(4)
XSAN_PERIODICAL_WRITE_CALLBACK(8)
XSAN_PERIODICAL_WRITE_CALLBACK(16)

/// TODO: use a macro to perform the ASan check for better performance?
#define XSAN_READ(size)                                \
  SANITIZER_INTERFACE_ATTRIBUTE                        \
  void __xsan_read##size(const void *p, uptr pc = 0) { \
    ScopedRealPc scoped_pc(pc);                        \
    XSAN_HOOKS_EXEC(__xsan_read<size>, (uptr)p);       \
  }

#define XSAN_WRITE(size)                                \
  SANITIZER_INTERFACE_ATTRIBUTE                         \
  void __xsan_write##size(const void *p, uptr pc = 0) { \
    ScopedRealPc scoped_pc(pc);                         \
    XSAN_HOOKS_EXEC(__xsan_write<size>, (uptr)p);       \
  }

XSAN_READ(1)
XSAN_READ(2)
XSAN_READ(4)
XSAN_READ(8)
XSAN_READ(16)
XSAN_WRITE(1)
XSAN_WRITE(2)
XSAN_WRITE(4)
XSAN_WRITE(8)
XSAN_WRITE(16)
}
