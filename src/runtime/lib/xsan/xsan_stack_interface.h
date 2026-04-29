// Offer sub-sanitizers a unified interface to get stack traces.
#pragma once

#include <sanitizer_common/sanitizer_flags.h>
#include <sanitizer_common/sanitizer_stacktrace.h>

#include "xsan_attribute.h"

namespace __asan {
u32 GetMallocContextSize();
}  // namespace __asan

namespace __xsan {

static const u32 kDefaultMallocContextSize = 30;

using __asan::GetMallocContextSize;

PSEUDO_MACRO void GetStackTrace(BufferedStackTrace& stack, uptr max_size,
                                bool fast) {
  if (max_size <= 2) {
    stack.size = max_size;
    if (max_size > 0) {
      stack.top_frame_bp = GET_CURRENT_FRAME();
      stack.trace_buffer[0] = StackTrace::GetCurrentPc();
      if (max_size > 1)
        stack.trace_buffer[1] = GET_CALLER_PC();
    }
  } else {
    stack.Unwind(StackTrace::GetCurrentPc(), GET_CURRENT_FRAME(), nullptr, fast,
                 max_size);
  }
}

PSEUDO_MACRO void GetStackTraceFatal(BufferedStackTrace& stack, uptr pc,
                                     uptr bp) {
  stack.Unwind(pc, bp, nullptr, common_flags()->fast_unwind_on_fatal);
}

PSEUDO_MACRO void GetStackTraceFatalHere(BufferedStackTrace& stack) {
  GetStackTrace(stack, kStackTraceMax, common_flags()->fast_unwind_on_fatal);
}

PSEUDO_MACRO void GetStackTraceThread(BufferedStackTrace& stack) {
  GetStackTrace(stack, kStackTraceMax, true);
}

PSEUDO_MACRO void GetStackTraceMalloc(BufferedStackTrace& stack) {
  GetStackTrace(stack, GetMallocContextSize(),
                common_flags()->fast_unwind_on_malloc);
}

PSEUDO_MACRO void GetStackTraceFree(BufferedStackTrace& stack) {
  GetStackTraceMalloc(stack);
}

PSEUDO_MACRO void PrintCurrentStack() {
  UNINITIALIZED BufferedStackTrace stack;
  GetStackTraceFatalHere(stack);
  stack.Print();
}

}  // namespace __xsan
