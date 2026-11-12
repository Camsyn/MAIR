// RUN: env MAIR=1 %clang -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - | FileCheck %s

// spin() only reads memory, so it cannot change what a check of *p reports.
// It may never return, though, and a check placed after the call would then
// never run. A later check stands in for an earlier access only if it is
// certain to execute, so the load keeps a check in front of the call.

__attribute__((noinline)) int spin(int *p) {
  while (*p) {
  }
  return 0;
}

int f(int *p) {
  int a = *p;
  int r = spin(p);
  *p = r;
  return a;
}

// CHECK-LABEL: define {{.*}} @f(
// CHECK: call void @__asan_report_load4
// CHECK: call {{.*}} @spin(
// CHECK: ret i32
