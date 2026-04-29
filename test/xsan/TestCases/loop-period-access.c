// clang-format off
// RUN: %clang_xsan -O2 -g -S -emit-llvm %s -o - -mllvm -xsan-loop-opt=range | FileCheck %s --check-prefix=IR-RANGE
// RUN: %clang_xsan -O2 -g -S -emit-llvm %s -o - -mllvm -xsan-loop-opt=period | FileCheck %s --check-prefix=IR-PERIOD
// RUN: %clang_xsan -O2 -g -S -emit-llvm %s -o - -mllvm -xsan-loop-opt=invariant | FileCheck %s --check-prefix=IR-INVARIANT
// RUN: %clang_xsan -O2 -g -S -emit-llvm %s -o - -mllvm -xsan-loop-opt=full | FileCheck %s --check-prefix=IR-FULL
// RUN: %clang_xsan -O2 -g %s -o %t -mllvm -xsan-loop-opt=full
// RUN: %run %t safe 2>&1 | FileCheck %s --check-prefix=SAFE
// RUN: not %run %t range-read 2>&1 | FileCheck %s --check-prefix=RANGE-READ
// RUN: not %run %t range-write 2>&1 | FileCheck %s --check-prefix=RANGE-WRITE
// RUN: not %run %t period-write 2>&1 | FileCheck %s --check-prefix=PERIOD-WRITE
// RUN: not %run %t invariant-write 2>&1 | FileCheck %s --check-prefix=INVARIANT-WRITE
// RUN: not %run %t while-write 2>&1 | FileCheck %s --check-prefix=WHILE-WRITE
// clang-format on

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 8

char global[SIZE];
char global_nozero[6] = {1, 1, 1, 1, 1, 1};
volatile int sink;

__attribute__((noinline)) int range_read_loop(char *s) {
  int len = 0;
  while (*s) {
    ++len;
    ++s;
  }
  return len;
}

__attribute__((noinline)) void range_write_for(char *p, int n) {
  for (int i = 0; i < n; ++i) {
    p[i] = 1;
  }
}

__attribute__((noinline)) void period_write_for(char *p, int n) {
  for (int i = 0; i < n; ++i) {
    p[i * 2] = 1;
  }
}

__attribute__((noinline)) void invariant_write_for(char *p, int n) {
  char *q = p + SIZE;
  for (int i = 0; i < n; ++i) {
    *q = (char)i;
    sink += i;
  }
}

__attribute__((noinline)) void counted_while_loop(char *p, int n) {
  while (n > 0) {
    p[n - 1] = 2;
    --n;
  }
}

__attribute__((noinline)) void guarded_for_loop(char *p, int n) {
  for (int i = 0; i < n; ++i) {
    if (i >= SIZE)
      continue;
    p[i] = 3;
  }
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "safe";

  if (strcmp(mode, "range-read") == 0)
    return range_read_loop(global_nozero);

  if (strcmp(mode, "range-write") == 0) {
    range_write_for(global, SIZE + 1);
    return 0;
  }

  if (strcmp(mode, "period-write") == 0) {
    period_write_for(global, 5);
    return 0;
  }

  if (strcmp(mode, "invariant-write") == 0) {
    invariant_write_for(global, 2);
    return 0;
  }

  if (strcmp(mode, "while-write") == 0) {
    counted_while_loop(global, SIZE + 1);
    return 0;
  }

  char local[] = "abc";
  range_read_loop(local);
  range_write_for(global, SIZE);
  period_write_for(global, SIZE / 2);
  counted_while_loop(global, SIZE);
  guarded_for_loop(global, SIZE + 5);
  printf("Done\n");
  return 0;
}

// IR-RANGE-LABEL: define {{.*}}@range_read_loop(
// IR-RANGE: call void @__xsan_read_range({{.*}}), !dbg
// IR-RANGE-LABEL: define {{.*}}@range_write_for(
// IR-RANGE: call void @__xsan_write_range({{.*}}), !dbg

// IR-PERIOD-LABEL: define {{.*}}@range_read_loop(
// IR-PERIOD: call void @__xsan_read_range({{.*}}), !dbg
// IR-PERIOD-LABEL: define {{.*}}@range_write_for(
// IR-PERIOD: call void @__xsan_write_range({{.*}}), !dbg
// IR-PERIOD-LABEL: define {{.*}}@period_write_for(
// IR-PERIOD: call void @__xsan_period_write1({{.*}}), !dbg

// IR-INVARIANT-LABEL: define {{.*}}@invariant_write_for(
// IR-INVARIANT: call void @__xsan_write1({{.*}}), !dbg

// IR-FULL-LABEL: define {{.*}}@range_read_loop(
// IR-FULL: call void @__xsan_read_range({{.*}}), !dbg
// IR-FULL-LABEL: define {{.*}}@range_write_for(
// IR-FULL: call void @__xsan_write_range({{.*}}), !dbg
// IR-FULL-LABEL: define {{.*}}@period_write_for(
// IR-FULL: call void @__xsan_period_write1({{.*}}), !dbg
// IR-FULL-LABEL: define {{.*}}@invariant_write_for(
// IR-FULL: call void @__xsan_write1({{.*}}), !dbg

// SAFE-NOT: ERROR: AddressSanitizer
// SAFE: Done

// RANGE-READ: ERROR: AddressSanitizer: global-buffer-overflow
// RANGE-READ: #0 {{.*}}__xsan_read_range
// RANGE-READ: #1 {{.*}}range_read_loop

// RANGE-WRITE: ERROR: AddressSanitizer: global-buffer-overflow
// RANGE-WRITE: #0 {{.*}}__xsan_write_range
// RANGE-WRITE: #1 {{.*}}range_write_for

// PERIOD-WRITE: ERROR: AddressSanitizer: global-buffer-overflow
// PERIOD-WRITE: #0 {{.*}}__xsan_period_write1
// PERIOD-WRITE: #1 {{.*}}period_write_for

// INVARIANT-WRITE: ERROR: AddressSanitizer: global-buffer-overflow
// INVARIANT-WRITE: #0 {{.*}}__xsan_write1
// INVARIANT-WRITE: #1 {{.*}}invariant_write_for

// WHILE-WRITE: ERROR: AddressSanitizer: global-buffer-overflow
// WHILE-WRITE: #{{[0-9]+}} {{.*}}counted_while_loop
