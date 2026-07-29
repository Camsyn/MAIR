// RUN: env MAIR=1 %clangxx_tsan -O0 -S -emit-llvm %s -o - | FileCheck %s

int update_value(int *p) {
  int old = *p;
  *p = old + 1;
  return old;
}

// CHECK-LABEL: define {{.*}} @_Z12update_valuePi
// CHECK: call void @__tsan_write4
// CHECK-NOT: call void @__tsan_read4
// CHECK-NOT: call void @__tsan_read_write4
