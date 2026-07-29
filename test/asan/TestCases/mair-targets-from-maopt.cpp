// RUN: env MAIR=1 %clangxx -asan -O0 -mllvm -asan-opt=false -mllvm -asan-max-ins-per-bb=1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clangxx -asan -O0 -mllvm -asan-instrument-reads=false -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NO-READ

int load_two_addresses(int *p, int *q) { return *p + *q; }

// CHECK-LABEL: define {{.*}} @_Z18load_two_addressesPiS_
// CHECK-COUNT-2: call void @__asan_report_load4

// NO-READ-LABEL: define {{.*}} @_Z18load_two_addressesPiS_
// NO-READ-NOT: call void @__asan_report_load4
