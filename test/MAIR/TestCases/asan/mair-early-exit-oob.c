// A loop that leaves on a data-dependent test has no trip count SCEV can
// compute; MAIR reads the count off an induction variable at the exit and
// checks everything the loop touched there, once. The check comes after the
// accesses: an overflow is reported at the exit, or the program faults first
// (docs/mair-loops.md, 数据相关的出口). Overflowing reads and writes
// must both be reported, and in-bounds runs must stay quiet. The IR checks at
// the end make sure the loops really go through the exit checks.
//
// RUN: env MAIR=1 %clang -asan -O2 -g %s -o %t
// RUN: %t until-in
// RUN: %t sum-in
// RUN: %t fill-in
// RUN: %t copy-in
// RUN: not %t until-oob 2>&1 | FileCheck %s --check-prefix=UNTIL
// RUN: not %t sum-oob 2>&1 | FileCheck %s --check-prefix=SUM
// RUN: not %t fill-oob 2>&1 | FileCheck %s --check-prefix=FILL
// RUN: not %t copy-oob 2>&1 | FileCheck %s --check-prefix=COPY
// RUN: env MAIR=1 %clang -asan -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -asan -O2 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STATS

#include <sanitizer/asan_interface.h>
#include <stdlib.h>
#include <string.h>

// The load decides when to leave: it runs until it reads a negative value.
__attribute__((noinline)) long until_neg(const long *a) {
  long i = 0;
  while (a[i] >= 0)
    i++;
  return i;
}
// UNTIL: ERROR: AddressSanitizer: use-after-poison
// UNTIL: READ of size
// UNTIL: #0 {{.*}} in until_neg {{.*}}mair-early-exit-oob.c:[[@LINE-6]]

// key decides when to leave; a is read once per iteration.
__attribute__((noinline)) long sum_until(const long *key, const long *a) {
  long s = 0;
  for (long i = 0; key[i] >= 0; i++)
    s += a[i];
  return s;
}
// SUM: ERROR: AddressSanitizer: heap-buffer-overflow
// SUM: READ of size
// SUM: #0 {{.*}} in sum_until {{.*}}mair-early-exit-oob.c:[[@LINE-5]]

// Writes are deferred too: an overflowing write lands before its check runs.
__attribute__((noinline)) void fill_until(const long *key, long *d) {
  for (long i = 0; key[i] >= 0; i++)
    d[i] = i + 1;
}
// FILL: ERROR: AddressSanitizer: heap-buffer-overflow
// FILL: WRITE of size
// FILL: #0 {{.*}} in fill_until {{.*}}mair-early-exit-oob.c:[[@LINE-4]]

// Only pointer induction variables: the count is their byte distance.
__attribute__((noinline)) void copy_until(long *d, const long *s) {
  while ((*d++ = *s++) != 0)
    ;
}
// COPY: ERROR: AddressSanitizer: heap-buffer-overflow
// COPY: WRITE of size
// COPY: #0 {{.*}} in copy_until {{.*}}mair-early-exit-oob.c:[[@LINE-5]]

// A call that writes memory is a barrier: the check stays in the loop.
static volatile int sink;
__attribute__((noinline)) void opaque(void) { sink = 1; }
__attribute__((noinline)) long until_call(const long *a) {
  long i = 0;
  while (a[i] >= 0) {
    opaque();
    i++;
  }
  return i;
}

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
  if (!strcmp(mode, "until-in") || !strcmp(mode, "until-oob")) {
    // a[5] stops the loop. until-oob poisons a[4..8): the loop reads a[4]
    // and a[5] before it leaves.
    a = make(8, 5);
    if (!strcmp(mode, "until-oob"))
      __asan_poison_memory_region(a + 4, 4 * sizeof(long));
    r = until_neg(a);
  } else if (!strcmp(mode, "sum-in") || !strcmp(mode, "sum-oob")) {
    // a[6] stops the loop: it reads b[0..6). sum-oob gives b only 4.
    a = make(8, 6);
    b = make(!strcmp(mode, "sum-oob") ? 4 : 6, -1);
    r = sum_until(a, b);
  } else if (!strcmp(mode, "fill-in") || !strcmp(mode, "fill-oob")) {
    // a[6] stops the loop: it writes b[0..6). fill-oob gives b only 4.
    a = make(8, 6);
    b = (long *)malloc((!strcmp(mode, "fill-oob") ? 4 : 6) * sizeof(long));
    fill_until(a, b);
    r = b[3];
  } else if (!strcmp(mode, "copy-in") || !strcmp(mode, "copy-oob")) {
    // a holds 6 nonzero values and a 0: 7 copies. copy-oob gives b only 4.
    a = make(7, -1);
    a[6] = 0;
    b = (long *)malloc((!strcmp(mode, "copy-oob") ? 4 : 7) * sizeof(long));
    copy_until(b, a);
    r = b[3];
  } else {
    a = make(4, 3);
    r = until_call(a);
  }
  free(a);
  free(b);
  return r < 0;
}

// The data-dependent loops keep no per-iteration check: one range check at
// the exit, sized by the induction variable there.
// IR-LABEL: define {{.*}} @until_neg(
// IR-NOT:   __asan_report_load8
// IR:       call void @__asan_loadN(
// IR-NOT:   __asan_report_load8
// IR:       {{^}$}}
// IR-LABEL: define {{.*}} @sum_until(
// IR:       call void @__asan_loadN(
// IR:       call void @__asan_loadN(
// IR-NOT:   __asan_report_load8
// IR:       {{^}$}}
// IR-LABEL: define {{.*}} @fill_until(
// IR:       call void @__asan_storeN(
// IR-NOT:   __asan_report_store8
// IR:       {{^}$}}
// IR-LABEL: define {{.*}} @copy_until(
// IR-NOT:   __asan_report_
// IR:       call void @__asan_load_periodic(
// IR:       call void @__asan_store_periodic(
// IR-NOT:   __asan_report_
// IR:       {{^}$}}
// A barrier keeps the check in the loop.
// IR-LABEL: define {{.*}} @until_call(
// IR-NOT:   __asan_loadN
// IR:       __asan_report_load8
// IR-NOT:   __asan_loadN
// IR:       {{^}$}}

// Seven summaries read their trip count off an induction variable; the load
// in until_call stays behind its barrier.
// STATS: lpoc{{ +}}in 28  range 5  periodic 2  exit-trip 7  rej-barrier 1{{$}}
