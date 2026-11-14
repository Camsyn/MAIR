// Interleaved strided reads merge into one periodic check with a smaller
// stride and more trips. The merged check must still catch every overflow of
// the original accesses and must not report in-bounds code or poisoned bytes
// that only lie in the gaps between the accesses. The IR checks at the end
// make sure the reads really go through a single merged periodic check.
//
// RUN: env MAIR=1 %clang -asan -O1 -g %s -o %t
// RUN: %t pair-in
// RUN: %t pair-gap
// RUN: %t three-in
// RUN: %t down-in
// RUN: not %t pair-oob 2>&1 | FileCheck %s --check-prefix=PAIR
// RUN: not %t three-oob 2>&1 | FileCheck %s --check-prefix=THREE
// RUN: not %t down-oob 2>&1 | FileCheck %s --check-prefix=DOWN
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR

#include <sanitizer/asan_interface.h>
#include <stdlib.h>
#include <string.h>

// p[4i] and p[4i+2]: stride 16 at +0 and +8, merged into stride 8.
__attribute__((noinline)) int sum_pair(int *p, int n) {
  int t = 0;
  for (int i = 0; i < n; ++i)
    t += p[4 * i] + p[4 * i + 2];
  return t;
}

// p[6i], p[6i+2] and p[6i+4]: stride 24 at +0, +8 and +16, merged into
// stride 8.
__attribute__((noinline)) int sum_three(int *p, int n) {
  int t = 0;
  for (int i = 0; i < n; ++i)
    t += p[6 * i] + p[6 * i + 2] + p[6 * i + 4];
  return t;
}

// The pair walked downward: stride -16, merged into stride -8 from the
// highest start.
__attribute__((noinline)) int sum_pair_down(int *p, int n) {
  int t = 0;
  for (int i = n - 1; i >= 0; --i)
    t += p[4 * i] + p[4 * i + 2];
  return t;
}

int main(int argc, char **argv) {
  const char *mode = argv[1];
  // sum_pair(p, 4) and sum_pair_down(p, 4) read up to p[14].
  int *p = (int *)calloc(15, sizeof(int));
  // sum_three(q, 4) reads up to q[22].
  int *q = (int *)calloc(23, sizeof(int));
  int r = 0;

  if (!strcmp(mode, "pair-in"))
    r = sum_pair(p, 4);
  if (!strcmp(mode, "pair-gap")) {
    // Poison every odd element, which no read touches.
    for (int i = 1; i < 15; i += 2)
      ASAN_POISON_MEMORY_REGION(p + i, sizeof(int));
    r = sum_pair(p, 4);
  }
  // PAIR: ERROR: AddressSanitizer: heap-buffer-overflow
  // PAIR: READ of size 4
  // PAIR: #0 {{.*}} in sum_pair {{.*}}mair-interleaved-runtime.c
  // PAIR: is located 0 bytes after 60-byte region
  if (!strcmp(mode, "pair-oob"))
    r = sum_pair(p + 1, 4);

  if (!strcmp(mode, "three-in"))
    r = sum_three(q, 4);
  // THREE: ERROR: AddressSanitizer: heap-buffer-overflow
  // THREE: READ of size 4
  // THREE: #0 {{.*}} in sum_three {{.*}}mair-interleaved-runtime.c
  // THREE: is located 0 bytes after 92-byte region
  if (!strcmp(mode, "three-oob"))
    r = sum_three(q + 1, 4);

  if (!strcmp(mode, "down-in"))
    r = sum_pair_down(p, 4);
  // DOWN: ERROR: AddressSanitizer: heap-buffer-overflow
  // DOWN: READ of size 4
  // DOWN: #0 {{.*}} in sum_pair_down {{.*}}mair-interleaved-runtime.c
  // DOWN: is located 0 bytes after 60-byte region
  if (!strcmp(mode, "down-oob"))
    r = sum_pair_down(p + 1, 4);

  free(q);
  free(p);
  return r;
}

// IR-LABEL: define {{.*}} @sum_pair(
// IR:       call void @__asan_load_periodic(i64 {{.*}}, i64 8, i64 4, i64 {{.*}})
// IR-NOT:   call void @__asan_load
// IR-LABEL: define {{.*}} @sum_three(
// IR:       call void @__asan_load_periodic(i64 {{.*}}, i64 8, i64 4, i64 {{.*}})
// IR-NOT:   call void @__asan_load
// IR-LABEL: define {{.*}} @sum_pair_down(
// IR:       call void @__asan_load_periodic(i64 {{.*}}, i64 -8, i64 4, i64 {{.*}})
// IR-NOT:   call void @__asan_load
// IR-LABEL: define {{.*}} @main(
