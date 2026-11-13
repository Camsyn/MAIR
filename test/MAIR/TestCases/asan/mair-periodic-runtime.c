// __asan_load_periodic / __asan_store_periodic check the footprint of one loop
// access over all iterations: the bytes [start + i * stride, start + i * stride
// + size) for 0 <= i < count. MAIR lowers periodic observations to these
// calls. This test calls them directly to pin down the edge cases.
//
// RUN: %clang -asan -O0 %s -o %t
// RUN: %t ok
// RUN: not %t up 2>&1 | FileCheck %s --check-prefix=UP
// RUN: not %t down 2>&1 | FileCheck %s --check-prefix=DOWN
// RUN: not %t strided 2>&1 | FileCheck %s --check-prefix=STRIDED
// RUN: not %t wide 2>&1 | FileCheck %s --check-prefix=WIDE
// RUN: not %t store 2>&1 | FileCheck %s --check-prefix=STORE
// RUN: env ASAN_OPTIONS=halt_on_error=0 %t recover 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RECOVER

#include <sanitizer/asan_interface.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void __asan_load_periodic(uintptr_t start, intptr_t stride, uintptr_t size,
                          uintptr_t count);
void __asan_store_periodic(uintptr_t start, intptr_t stride, uintptr_t size,
                           uintptr_t count);
void __asan_load_periodic_noabort(uintptr_t start, intptr_t stride,
                                  uintptr_t size, uintptr_t count);

int main(int argc, char **argv) {
  const char *mode = argv[1];
  char *buf = (char *)malloc(64);
  uintptr_t b = (uintptr_t)buf;

  if (!strcmp(mode, "ok")) {
    // A zero trip count checks nothing, wherever start points.
    __asan_load_periodic(b + 1000, 4, 4, 0);
    __asan_load_periodic(b - 1000, -4, 4, 0);
    // In bounds: ascending, descending, overlapping and strided.
    __asan_load_periodic(b, 4, 4, 16);
    __asan_load_periodic(b + 60, -4, 4, 16);
    __asan_load_periodic(b, 2, 4, 31);
    __asan_load_periodic(b + 4, 8, 4, 8);
    __asan_load_periodic(b + 60, -8, 4, 8);
    __asan_store_periodic(b, 16, 16, 4);
    // Only the gaps between the accessed bytes are poisoned. The hull of the
    // footprint is not clean, but no access touches a poisoned byte.
    for (int i = 0; i < 4; ++i)
      ASAN_POISON_MEMORY_REGION(buf + 16 * i, 8);
    __asan_load_periodic(b + 8, 16, 8, 4);
    __asan_load_periodic(b + 56, -16, 8, 4);
    // A stride far wider than the access skips the hull and checks each one.
    __asan_load_periodic(b + 8, 1 << 20, 8, 1);
    ASAN_UNPOISON_MEMORY_REGION(buf, 64);
    fprintf(stderr, "ok\n");
    free(buf);
    return 0;
  }

  // UP: READ of size 4 at [[ADDR:0x[0-9a-f]+]]
  // UP: [[ADDR]] is located 0 bytes after 64-byte region
  if (!strcmp(mode, "up"))
    __asan_load_periodic(b + 4, 4, 4, 16);

  // The low end of a descending footprint is start - (count - 1) * |stride|.
  // DOWN: READ of size 4
  // DOWN: is located 4 bytes before 64-byte region
  if (!strcmp(mode, "down"))
    __asan_load_periodic(b + 56, -4, 4, 16);

  // STRIDED: READ of size 2
  // STRIDED: is located 0 bytes after 64-byte region
  if (!strcmp(mode, "strided"))
    __asan_load_periodic(b + 0, 32, 2, 3);

  // A stride far wider than the access, past the hull threshold. The second
  // access straddles the end of a 4108-byte buffer.
  // WIDE: READ of size 8
  // WIDE: is located 0 bytes after 4108-byte region
  if (!strcmp(mode, "wide")) {
    char *big = (char *)malloc(4108);
    __asan_load_periodic((uintptr_t)big + 8, 4096, 8, 2);
  }

  // STORE: WRITE of size 8
  // STORE: is located 0 bytes after 64-byte region
  if (!strcmp(mode, "store"))
    __asan_store_periodic(b + 48, 8, 8, 3);

  // RECOVER: READ of size 4
  // RECOVER: continued
  if (!strcmp(mode, "recover")) {
    __asan_load_periodic_noabort(b + 4, 4, 4, 16);
    fprintf(stderr, "continued\n");
    free(buf);
    return 0;
  }

  free(buf);
  return 0;
}
