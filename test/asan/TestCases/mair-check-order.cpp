// RUN: env MAIR=1 %clangxx -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - | FileCheck %s

int f(int *p, int *q) { return *p + *q; }

// CHECK-LABEL: define {{.*}} @_Z1f
// CHECK: call void @__asan_report_load4
// CHECK: load i32
// CHECK: call void @__asan_report_load4
// CHECK: load i32
