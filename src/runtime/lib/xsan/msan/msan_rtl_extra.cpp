#include "../xsan_common_defs.h"
#include "orig/msan_interface_internal.h"

extern "C" {

/// ASan writes the current fake stack through `fake_stack_save` inside
/// __sanitizer_start_switch_fiber().  In XSan's ASan+MSan composition this
/// write is invisible to MSan shadow, so the later
/// __sanitizer_finish_switch_fiber(fake_stack_save, ...) can otherwise report
/// a false use-of-uninitialized-value.  Unpoison only the out-param slot after
/// the real ASan runtime has populated it.
XSAN_WRAPPER(void, __sanitizer_start_switch_fiber, void **fake_stack_save,
             const void *bottom, uptr size) {
  XSAN_REAL(__sanitizer_start_switch_fiber)(fake_stack_save, bottom, size);
  if (fake_stack_save)
    __msan_unpoison(fake_stack_save, sizeof(*fake_stack_save));
}

}  // extern "C"
