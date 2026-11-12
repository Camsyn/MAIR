// RUN: env MAIR=1 %clangxx -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - | FileCheck %s

#include <atomic>

int f(std::atomic<int> *p) {
  return p->load(std::memory_order_relaxed) +
         p->load(std::memory_order_relaxed);
}

// The two loads need one check. Atomic observations stay at their access, and
// the second, which is certain to run, covers the first.
// CHECK-LABEL: define {{.*}} @_Z1f
// CHECK: load atomic i32
// CHECK-COUNT-1: call void @__asan_report_load4
// CHECK: load atomic i32
// CHECK-NOT: call void @__asan_report_load4
// CHECK: ret i32
