// A loop whose index is an unsigned char: it wraps after 256 iterations and
// revisits a[0..]. MAIR checks the accesses once at the exit, as the
// min(T, 256) elements from a (docs/mair-loops.md, 窄下标回环访问). The loops
// run 300 times: an array of 256 elements is in bounds and must stay quiet (a
// summary of 300 elements would report it), an array of 255 overflows and
// must be reported. sum_u8 and fill_u8 keep one access per iteration;
// fill_unrolled is unrolled 4 times by -O2 (four phases of step 4, merged
// after LPOC) and sum_down counts down from 255.
//
// RUN: env MAIR=1 %clang -asan -O2 -g %s -o %t
// RUN: %t read 256
// RUN: %t write 256
// RUN: not %t read 255 2>&1 | FileCheck %s --check-prefix=READ
// RUN: not %t write 255 2>&1 | FileCheck %s --check-prefix=WRITE
// RUN: %t unrolled 256
// RUN: %t down 256
// RUN: not %t unrolled 255 2>&1 | FileCheck %s --check-prefix=UNROLLED
// RUN: not %t down 255 2>&1 | FileCheck %s --check-prefix=DOWN
// RUN: env MAIR=1 %clang -asan -O2 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -asan -O2 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STATS

#include <stdlib.h>
#include <string.h>

__attribute__((noinline)) unsigned sum_u8(const unsigned char *a, int n) {
  unsigned s = 0;
  unsigned char i = 0;
#pragma clang loop unroll(disable) vectorize(disable)
  for (int k = 0; k < n; k++, i++)
    s += a[i];
  return s;
}
// READ: ERROR: AddressSanitizer: heap-buffer-overflow
// READ: READ of size 256
// READ: #0 {{.*}} in sum_u8 {{.*}}mair-wrapping-index-oob.c:[[@LINE-5]]

__attribute__((noinline)) void fill_u8(int *a, int n) {
  unsigned char i = 0;
#pragma clang loop unroll(disable) vectorize(disable)
  for (int k = 0; k < n; k++, i++)
    a[i] = k;
}
// WRITE: ERROR: AddressSanitizer: heap-buffer-overflow
// WRITE: WRITE of size 1024
// WRITE: #0 {{.*}} in fill_u8 {{.*}}mair-wrapping-index-oob.c:[[@LINE-4]]

__attribute__((noinline)) void fill_unrolled(int *a, int n) {
  unsigned char i = 0;
#pragma clang loop vectorize(disable)
  for (int k = 0; k < n; k++, i++)
    a[i] = k;
}
// UNROLLED: ERROR: AddressSanitizer: heap-buffer-overflow
// UNROLLED: WRITE of size 16
// UNROLLED: #0 {{.*}} in fill_unrolled {{.*}}mair-wrapping-index-oob.c:[[@LINE-4]]

// Reads a[255], a[254], ..., a[0], a[255], ...
__attribute__((noinline)) unsigned sum_down(const unsigned char *a, int n) {
  unsigned s = 0;
  unsigned char i = 255;
#pragma clang loop unroll(disable) vectorize(disable)
  for (int k = 0; k < n; k++, i--)
    s += a[i];
  return s;
}
// DOWN: ERROR: AddressSanitizer: heap-buffer-overflow
// DOWN: READ of size 1
// DOWN: #0 {{.*}} in sum_down {{.*}}mair-wrapping-index-oob.c:[[@LINE-5]]

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  int len = atoi(argv[2]);
  unsigned r = 0;
  if (!strcmp(argv[1], "read") || !strcmp(argv[1], "down")) {
    unsigned char *a = (unsigned char *)malloc(len);
    memset(a, 1, len);
    r = !strcmp(argv[1], "read") ? sum_u8(a, 300) : sum_down(a, 300);
    free(a);
  } else {
    int *a = (int *)malloc(len * sizeof(int));
    if (!strcmp(argv[1], "write"))
      fill_u8(a, 300);
    else
      fill_unrolled(a, 300);
    r = a[0];
    free(a);
  }
  return r == 12345;
}

// No per-iteration check; one check at the exit.
// IR-LABEL: define {{.*}} @sum_u8(
// IR-NOT:   __asan_report_load1
// IR:       call void @__asan_loadN(
// IR-NOT:   __asan_report_load1
// IR:       {{^}$}}
// IR-LABEL: define {{.*}} @fill_u8(
// IR-NOT:   __asan_report_store4
// IR:       call void @__asan_storeN(
// IR-NOT:   __asan_report_store4
// IR:       {{^}$}}

// IR-LABEL: define {{.*}} @fill_unrolled(
// IR-NOT:   __asan_report_store4
// IR:       call void @__asan_store{{N|_periodic}}(
// IR:       {{^}$}}
// IR-LABEL: define {{.*}} @sum_down(
// IR-NOT:   __asan_report_load1
// IR:       call void @__asan_load_periodic(
// IR-NOT:   __asan_report_load1
// IR:       {{^}$}}

// fill_unrolled: four phases of the unrolled loop; its epilogue loop starts
// at the symbolic 4T and stays in the loop (rej-wrap).
// STATS: lpoc{{ +}}in {{[0-9]+}}  {{.*}}wrap-index 7  rej-wrap 1{{$}}
