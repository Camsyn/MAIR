// An out-of-bounds volatile store lowered through MAIR is still reported.
// RUN: env MAIR=1 %clang -asan -O1 -mair-stats %s -o %t 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STATS
// RUN: env MAIR=1 not %t 2>&1 | FileCheck %s

#include <stdlib.h>

int main(void) {
  volatile char *a = malloc(8);
  a[8] = 1;
  free((void *)a);
  return 0;
}

// STATS: select{{ +}}in 1  lowered 1{{$}}
// CHECK: ERROR: AddressSanitizer: heap-buffer-overflow
// CHECK: WRITE of size 1
