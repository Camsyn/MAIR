// clang-format off
// RUN: %clang_xsan -O2 -g -S -emit-llvm %s -o - -mllvm -xsan-loop-opt=range | FileCheck %s --check-prefix=IR
// RUN: %clang_xsan -O2 -g %s -o %t -mllvm -xsan-loop-opt=range
// RUN: not %run %t 2>&1 | FileCheck %s --check-prefix=REPORT
// clang-format on

char global_nozero[6] = {1, 1, 1, 1, 1, 1};

__attribute__((noinline)) int trigger(char *s) {
  int len = 0;
  // clang-format off
  // REPORT: #1 {{.*}} in trigger {{.*}}loop-report-location-range.c:[[@LINE+2]]:{{[0-9]+}}
  // clang-format on
  while (*s) {
    ++len;
    ++s;
  }
  return len;
}

int main(void) { return trigger(global_nozero); }

// IR-LABEL: define {{.*}}@trigger(
// IR: call void @__xsan_read_range({{.*}}), !dbg
