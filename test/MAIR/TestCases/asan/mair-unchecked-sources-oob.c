// A load native ASan never checks is dropped before MAIR optimization, so the
// checked loads next to it merge into one range check. The range check must
// still catch an overflow of the original loads.
//
// RUN: env MAIR=1 %clang -asan -O1 -g %s -o %t
// RUN: %t 5
// RUN: not %t 7 2>&1 | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR

#include <stdlib.h>

__attribute__((noinline)) void fill(char *p) {
  for (int k = 0; k < 8; ++k)
    p[k] = (char)k;
}

// Stack safety proves buf[i] in bounds, so native ASan does not check it.
// buf[i + 1] and buf[i + 2] overflow for i == 7; MAIR checks them as one
// 2-byte range.
// IR-LABEL: define {{.*}} @sum3(
// IR-NOT:   call void @__asan_report_load1(
// IR:       call void @__asan_report_load_n(i64 %{{.*}}, i64 2)
// IR-NOT:   call void @__asan_report_load1(
// IR:       ret i32
__attribute__((noinline)) int sum3(long n) {
  char buf[8];
  fill(buf);
  long i = n & 7;
  // CHECK: ERROR: AddressSanitizer: stack-buffer-overflow
  // CHECK: READ of size 2
  // CHECK: #0 {{.*}} in sum3 {{.*}}mair-unchecked-sources-oob.c:[[@LINE+1]]
  return buf[i] + buf[i + 1] + buf[i + 2];
}

volatile int Sink;

int main(int argc, char **argv) {
  Sink = sum3(atol(argv[1]));
  return 0;
}
