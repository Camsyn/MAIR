// At -O0 TSan does not use MAIR: the output is the same as without MAIR, also
// when -disable-O0-optnone leaves the functions without optnone.
// RUN: env MAIR=1 %clang -tsan -O0 -S -emit-llvm %s -o %t.mair.ll
// RUN: env -u MAIR %clang -tsan -O0 -S -emit-llvm %s -o %t.native.ll
// RUN: diff %t.native.ll %t.mair.ll
// RUN: env MAIR=1 %clang -tsan -O0 -Xclang -disable-O0-optnone -S -emit-llvm \
// RUN:   %s -o %t.nooptnone.mair.ll
// RUN: env -u MAIR %clang -tsan -O0 -Xclang -disable-O0-optnone -S -emit-llvm \
// RUN:   %s -o %t.nooptnone.native.ll
// RUN: diff %t.nooptnone.native.ll %t.nooptnone.mair.ll
// At -O1 MAIR records the loop as one range.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s

int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i];
  return s;
}
// CHECK-LABEL: define {{.*}} @sum(
// CHECK: call void @__tsan_read_range(

void pair(long *p) {
  p[0] = 1;
  p[1] = 2;
}
