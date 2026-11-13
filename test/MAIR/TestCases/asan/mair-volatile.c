// MAIR treats a volatile access like any other: ASan lowers its observation
// through MAIR, and one check may stand in for a second access to the same
// bytes. Both volatile loads themselves stay in place.
// RUN: env MAIR=1 %clang -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=ONE
// RUN: env MAIR=1 %clang -asan -O1 -mllvm -asan-opt=false -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STATS

int twice(volatile int *p) { return *p + *p; }

// The check may follow the first load, as in mair-check-order.cpp.
// CHECK-LABEL: define {{.*}} @twice(
// CHECK-DAG:   call void @__asan_report_load4
// CHECK-DAG:   load volatile i32, ptr %p
// CHECK-DAG:   load volatile i32, ptr %p
// CHECK:       ret i32

// ONE:     call void @__asan_report_load4
// ONE-NOT: call void @__asan_report_load4

// STATS: MAIR stats
// STATS: select{{ +}}in 1  lowered 1{{$}}
