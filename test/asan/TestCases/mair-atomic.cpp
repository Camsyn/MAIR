// RUN: env MAIR=1 %clangxx -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - | FileCheck %s

#include <atomic>

int f(std::atomic<int> *p) {
  return p->load(std::memory_order_relaxed) +
         p->load(std::memory_order_relaxed);
}

// CHECK-LABEL: define {{.*}} @_Z1f
// CHECK-COUNT-1: call void @__asan_report_load4
// CHECK-COUNT-2: load atomic i32
