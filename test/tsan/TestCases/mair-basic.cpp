// RUN: env MAIR=1 %clangxx_tsan -O1 -S -emit-llvm %s -o - | FileCheck %s

int g;

void f(int *p) {
  g = *p;
}

// CHECK-LABEL: define {{.*}} @_Z1f
// CHECK: call void @__tsan_read4
// CHECK: call void @__tsan_write4
