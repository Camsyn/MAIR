// A data-dependent loop whose only integer counter is an unsigned char: it
// wraps after 256 iterations, so its value at the exit cannot count the
// iterations. Built with -fno-strict-overflow, the pointer induction
// variables carry no wrap flags either. MAIR must not read the trip count off
// any of them; it counts the iterations in a 64-bit counter it inserts itself
// (docs/mair-loops.md, 数据相关的出口). The loops run more than 256 times and
// overflow past element 256: the overflow must be reported, and in-bounds runs
// must stay quiet. The IR checks make sure the loops really go through the
// exit checks and the inserted counter.
//
// RUN: env MAIR=1 %clang -asan -O2 -g -fno-strict-overflow %s -o %t
// RUN: %t scan-in
// RUN: %t fill-in
// RUN: not %t scan-oob 2>&1 | FileCheck %s --check-prefix=SCAN
// RUN: not %t fill-oob 2>&1 | FileCheck %s --check-prefix=FILL
// RUN: env MAIR=1 %clang -asan -O2 -fno-strict-overflow -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -asan -O2 -fno-strict-overflow -mair-stats -c %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATS

#include <sanitizer/asan_interface.h>
#include <stdlib.h>
#include <string.h>

// Reads until a negative value; n counts the reads modulo 256.
__attribute__((noinline)) unsigned char scan_u8(const long *a) {
  unsigned char n = 0;
  while (*a >= 0) {
    a++;
    n++;
  }
  return n;
}
// SCAN: ERROR: AddressSanitizer: use-after-poison
// SCAN: READ of size
// SCAN: #0 {{.*}} in scan_u8 {{.*}}mair-exit-counter-oob.c:[[@LINE-8]]

// Copies the iteration number until key holds a negative value.
__attribute__((noinline)) unsigned char fill_u8(const long *key, long *d) {
  unsigned char n = 0;
  while (*key++ >= 0)
    *d++ = n++;
  return n;
}
// FILL: ERROR: AddressSanitizer: heap-buffer-overflow
// FILL: WRITE of size
// FILL: #0 {{.*}} in fill_u8 {{.*}}mair-exit-counter-oob.c:[[@LINE-5]]

static long *make(long n, long sentinel_at) {
  long *p = (long *)malloc(n * sizeof(long));
  for (long i = 0; i < n; i++)
    p[i] = i == sentinel_at ? -1 : i + 1;
  return p;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const char *mode = argv[1];
  long r = 0;
  long *a = 0, *b = 0;
  if (!strcmp(mode, "scan-in") || !strcmp(mode, "scan-oob")) {
    // a[300] stops the loop after 301 reads. scan-oob poisons a[290..400):
    // a count taken modulo 256 (45 reads) would not reach it.
    a = make(400, 300);
    if (!strcmp(mode, "scan-oob"))
      __asan_poison_memory_region(a + 290, 110 * sizeof(long));
    r = scan_u8(a);
  } else if (!strcmp(mode, "fill-in") || !strcmp(mode, "fill-oob")) {
    // a[300] stops the loop: it writes b[0..300). fill-oob gives b only 280.
    a = make(400, 300);
    b = (long *)malloc((!strcmp(mode, "fill-oob") ? 280 : 300) * sizeof(long));
    r = fill_u8(a, b);
    r += b[279];
  }
  free(a);
  free(b);
  return r < 0;
}

// No per-iteration check; one check at the exit, sized by a counter MAIR
// inserted into the loop header.
// IR-LABEL: define {{.*}} @scan_u8(
// IR:       %mair.count = phi i64
// IR-NOT:   __asan_report_load8
// IR:       call void @__asan_load_periodic(
// IR-NOT:   __asan_report_load8
// IR:       {{^}$}}
// IR-LABEL: define {{.*}} @fill_u8(
// IR:       %mair.count = phi i64
// IR-NOT:   %mair.count{{[0-9]+}} = phi
// IR-NOT:   __asan_report_
// IR:       call void @__asan_store_periodic(
// IR-NOT:   __asan_report_
// IR:       {{^}$}}

// Three summaries count with an inserted counter; the two of fill_u8 share
// its one counter (IR above). The ranges are main's initialization loops.
// STATS: lpoc{{ +}}in {{[0-9]+}}  range 2  periodic 3  exit-trip 3  exit-counter 3{{$}}
