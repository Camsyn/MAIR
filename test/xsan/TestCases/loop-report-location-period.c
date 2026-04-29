// clang-format off
// RUN: %clang_xsan -O2 -g -S -emit-llvm %s -o - -mllvm -xsan-loop-opt=period | FileCheck %s --check-prefix=IR
// RUN: %clang_xsan -O2 -g %s -o %t -mllvm -xsan-loop-opt=period
// RUN: not %run %t 2>&1 | FileCheck %s --check-prefix=REPORT
// clang-format on

#include <stdlib.h>

char global[8];

__attribute__((noinline)) void trigger(char *p, int n) {
  for (int i = 0; i < n; ++i) {
    // clang-format off
    // REPORT: #1 {{.*}} in trigger {{.*}}loop-report-location-period.c:[[@LINE+2]]:{{[0-9]+}}
    // clang-format on
    p[i * 2] = 1;
  }
}

int main(int argc, char **argv) {
  trigger(global, argc == 99 ? atoi(argv[0]) : 5);
  return 0;
}

// IR-LABEL: define {{.*}}@trigger(
// IR: call void @__xsan_period_write1({{.*}}), !dbg
