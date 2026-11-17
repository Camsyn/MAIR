// Object bounds elimination (-mair-asan-boe) deletes the checks of accesses
// that provably stay within a constant global. Constant data is never
// poisoned, so such a check can never fire.
//
// RUN: env MAIR=1 %clang -asan -O1 -mllvm -mair-asan-boe -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefixes=IR,DEFAULT
// RUN: env MAIR=1 %clang -asan -O1 -mllvm -mair-asan-boe -mair-stats -c %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATS
// RUN: env MAIR=1 %clang -asan -O1 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NO-STATS
// RUN: env MAIR=1 %clang -asan -O1 -g -mllvm -mair-asan-boe %s -o %t
// RUN: %t 255
// RUN: not %t 300 2>&1 | FileCheck %s --check-prefix=OOB

#include <stdlib.h>

static const unsigned char T[256] = {1, 2, 3, 4, 5, 6, 7, 8};
unsigned char M[256] = {1, 2, 3};

// T[(unsigned char)s[i]] stays within the 256 bytes of T, so only s[i] is
// checked, as one range at the loop exit. Without the option, T[...] is
// checked in the loop.
// IR-LABEL: define {{.*}} @lower_sum(
// IR-NOT:       call void @__asan_report_load1(
// IR:           call void @__asan_loadN(
// DEFAULT:      call void @__asan_report_load1(
// IR-NOT:       call void @__asan_report_load1(
// IR:           {{^}$}}
__attribute__((noinline)) int lower_sum(const char *s, int n) {
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += T[(unsigned char)s[i]];
  return sum;
}

// T[(unsigned char)s[i] + 1] reads T[256] for s[i] == 255: kept.
// IR-LABEL: define {{.*}} @plus_one(
// IR:           call void @__asan_loadN(
// IR:           call void @__asan_report_load1(
// IR-NOT:       call void @__asan_report_load1(
// IR:           {{^}$}}
__attribute__((noinline)) int plus_one(const char *s, int n) {
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += T[(unsigned char)s[i] + 1];
  return sum;
}

// The user may poison a mutable global: kept.
// IR-LABEL: define {{.*}} @mutable_sum(
// IR:           call void @__asan_loadN(
// IR:           call void @__asan_report_load1(
// IR-NOT:       call void @__asan_report_load1(
// IR:           {{^}$}}
__attribute__((noinline)) int mutable_sum(const char *s, int n) {
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += M[(unsigned char)s[i]];
  return sum;
}

// An unbounded index is kept, and an overflow is reported.
// IR-LABEL: define {{.*}} @lookup(
// IR:           call void @__asan_report_load1(
// IR:           {{^}$}}
__attribute__((noinline)) int lookup(int idx) {
  // OOB: ERROR: AddressSanitizer: global-buffer-overflow
  // OOB: READ of size 1
  // OOB: #0 {{.*}} in lookup {{.*}}mair-safe-const-table.c:[[@LINE+1]]
  return T[idx];
}

// The option adds boe after the second redundancy elimination.
// STATS:      roe#2
// STATS-NEXT: boe{{ +}}in {{[0-9]+}}  eliminated 1  scev 1{{$}}
// STATS-NEXT: simplify#2
// NO-STATS-NOT: boe

volatile int Sink;

int main(int argc, char **argv) {
  char s[4] = {'a', (char)255, 0, 'z'};
  Sink = lower_sum(s, 4) + plus_one(s, 1) + mutable_sum(s, 4);
  Sink = lookup(atoi(argv[1]));
  return 0;
}
