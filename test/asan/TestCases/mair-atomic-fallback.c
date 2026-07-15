// RUN: env MAIR=1 %clang -asan -O0 -S -emit-llvm %s -o - | FileCheck %s

int f(int *p) {
  return __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST);
}

// CHECK-LABEL: define {{.*}} @f(
// CHECK: call void @__asan_report_store4
// CHECK: atomicrmw add
