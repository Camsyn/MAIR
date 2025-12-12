#pragma once

#include <sanitizer_common/sanitizer_internal_defs.h>

using __sanitizer::u32;
using __sanitizer::u64;
using __sanitizer::uptr;

extern "C" {
// This function should be called at the very beginning of the process,
// before any instrumented code is executed and before any call to malloc.
SANITIZER_INTERFACE_ATTRIBUTE void __xsan_init();

// __asan_init has different semantics.
SANITIZER_INTERFACE_ATTRIBUTE void __xsan_asan_init();

/// FIXME: should this be deprecated?
// // Performs cleanup before a NoReturn function. Must be called before things
// // like _exit and execl to avoid false positives on stack.
// SANITIZER_INTERFACE_ATTRIBUTE void __xsan_handle_no_return();

SANITIZER_INTERFACE_ATTRIBUTE
const char *__xsan_default_options();

// This macro set visibility to default (i.e., not hidden), which export the
// external symbol to other module.
SANITIZER_INTERFACE_ATTRIBUTE
void *__xsan_memcpy(void *dest, const void *src, uptr count);
SANITIZER_INTERFACE_ATTRIBUTE
void *__xsan_memset(void *dest, int ch, uptr count);
SANITIZER_INTERFACE_ATTRIBUTE
void *__xsan_memmove(void *dest, const void *src, uptr count);
}
