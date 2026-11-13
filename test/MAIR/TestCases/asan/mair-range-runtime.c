// Merged and loop-range checks must still catch every overflow of the original
// accesses and must not report in-bounds code. Each mode runs one scenario on
// a 4-int heap buffer; the IR checks at the end make sure the scenarios really
// go through the MAIR range checks rather than native per-access checks.
//
// RUN: env MAIR=1 %clang -asan -O1 -g %s -o %t
// RUN: %t pair-in
// RUN: %t sum-in
// RUN: %t from-in
// RUN: %t down-in
// RUN: %t field-in
// RUN: %t padded-in
// RUN: not %t pair-oob 2>&1 | FileCheck %s --check-prefix=PAIR
// RUN: not %t sum-oob 2>&1 | FileCheck %s --check-prefix=SUM
// RUN: not %t from-oob 2>&1 | FileCheck %s --check-prefix=FROM
// RUN: not %t down-oob 2>&1 | FileCheck %s --check-prefix=DOWN-HIGH
// RUN: not %t down-under 2>&1 | FileCheck %s --check-prefix=DOWN-LOW
// RUN: not %t field-oob 2>&1 | FileCheck %s --check-prefix=FIELD
// RUN: not %t padded-oob 2>&1 | FileCheck %s --check-prefix=PADDED
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR

#include <sanitizer/asan_interface.h>
#include <stdlib.h>
#include <string.h>

// Merged into one 8-byte store range. The report shows the merged size and the
// location of the first access.
__attribute__((noinline)) void store_pair(int *p) {
  p[0] = 1;
  p[1] = 2;
}
// PAIR: ERROR: AddressSanitizer: heap-buffer-overflow
// PAIR: WRITE of size 8
// PAIR: #0 {{.*}} in store_pair {{.*}}mair-range-runtime.c:[[@LINE-5]]

// A contiguous range with a constant start.
__attribute__((noinline)) int sum(int *p, int n) {
  int t = 0;
  for (int i = 0; i < n; ++i)
    t += p[i];
  return t;
}

// A periodic region with a symbolic start.
__attribute__((noinline)) int sum_from(int *p, int b, int n) {
  int t = 0;
  for (int i = b; i < n; ++i)
    t += p[i];
  return t;
}

// A periodic region with a negative stride.
__attribute__((noinline)) int sum_down(int *p, int n) {
  int t = 0;
  for (int i = n - 1; i >= 0; --i)
    t += p[i];
  return t;
}

struct pair {
  int a, b;
};

// A strided periodic region: stride 8, access size 4.
__attribute__((noinline)) int sum_field(struct pair *p, int n) {
  int t = 0;
  for (int i = 0; i < n; ++i)
    t += p[i].b;
  return t;
}

struct padded {
  char pad[8];
  long b;
};

// Stride 16, access size 8. The pads lie between the accessed bytes.
__attribute__((noinline)) long sum_padded(struct padded *p, int n) {
  long t = 0;
  for (int i = 0; i < n; ++i)
    t += p[i].b;
  return t;
}

int main(int argc, char **argv) {
  const char *mode = argv[1];
  int *p = (int *)malloc(4 * sizeof(int));
  memset(p, 0, 4 * sizeof(int));
  struct pair *pairs = (struct pair *)calloc(2, sizeof(struct pair));
  struct padded *padded = (struct padded *)calloc(4, sizeof(struct padded));
  // Poison every pad. The hull of the b fields then contains poisoned bytes
  // that no access touches; reading the fields must not report them.
  for (int i = 0; i < 4; ++i)
    ASAN_POISON_MEMORY_REGION(padded[i].pad, sizeof(padded[i].pad));
  int r = 0;

  if (!strcmp(mode, "pair-in"))
    store_pair(p + 2);
  if (!strcmp(mode, "pair-oob"))
    store_pair(p + 3);

  if (!strcmp(mode, "sum-in"))
    r = sum(p, 4);
  // SUM: ERROR: AddressSanitizer: heap-buffer-overflow
  // SUM: READ of size 20
  // SUM: #0 {{.*}} in sum {{.*}}mair-range-runtime.c
  if (!strcmp(mode, "sum-oob"))
    r = sum(p, 5);

  if (!strcmp(mode, "from-in"))
    r = sum_from(p, 1, 4);
  // The runtime reports the first iteration that overflows, with the size of
  // one access, just like a per-iteration check would.
  // FROM: ERROR: AddressSanitizer: heap-buffer-overflow
  // FROM: READ of size 4
  // FROM: #0 {{.*}} in sum_from {{.*}}mair-range-runtime.c
  // FROM: is located 0 bytes after 16-byte region
  if (!strcmp(mode, "from-oob"))
    r = sum_from(p, 1, 5);

  if (!strcmp(mode, "down-in"))
    r = sum_down(p, 4);
  // DOWN-HIGH: ERROR: AddressSanitizer: heap-buffer-overflow
  // DOWN-HIGH: READ of size 4
  // DOWN-HIGH: is located 0 bytes after 16-byte region
  if (!strcmp(mode, "down-oob"))
    r = sum_down(p, 5);
  // DOWN-LOW: ERROR: AddressSanitizer: heap-buffer-overflow
  // DOWN-LOW: READ of size 4
  // DOWN-LOW: is located 4 bytes before 16-byte region
  if (!strcmp(mode, "down-under"))
    r = sum_down(p - 1, 4);

  if (!strcmp(mode, "field-in"))
    r = sum_field(pairs, 2);
  // FIELD: ERROR: AddressSanitizer: heap-buffer-overflow
  // FIELD: READ of size 4
  // FIELD: is located 4 bytes after 16-byte region
  if (!strcmp(mode, "field-oob"))
    r = sum_field(pairs, 3);

  if (!strcmp(mode, "padded-in"))
    r = (int)sum_padded(padded, 4);
  // PADDED: ERROR: AddressSanitizer: heap-buffer-overflow
  // PADDED: READ of size 8
  // PADDED: is located 8 bytes after 64-byte region
  if (!strcmp(mode, "padded-oob"))
    r = (int)sum_padded(padded, 5);

  free(padded);
  free(pairs);
  free(p);
  return r;
}

// IR-LABEL: define {{.*}} @store_pair(
// IR-NOT:   __asan_report_store4
// IR:       call void @__asan_report_store_n({{.*}}, i64 8)
// IR-LABEL: define {{.*}} @sum(
// IR:       call void @__asan_loadN(
// IR-LABEL: define {{.*}} @sum_from(
// IR:       call void @__asan_load_periodic(
// IR-LABEL: define {{.*}} @sum_down(
// IR:       call void @__asan_load_periodic(
// IR-LABEL: define {{.*}} @sum_field(
// IR:       call void @__asan_load_periodic(
// IR-LABEL: define {{.*}} @sum_padded(
// IR:       call void @__asan_load_periodic(
