// RUN: env MAIR=1 %clangxx -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - | FileCheck %s

int f(int *p, int *q) { return *p + *q; }

// The actual load may happen before its ASan check. Both addresses stay
// instrumented.
// CHECK-LABEL: define {{.*}} @_Z1f
// CHECK-COUNT-2: call void @__asan_report_load4
