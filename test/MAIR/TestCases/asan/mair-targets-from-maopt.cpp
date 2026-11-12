// RUN: env MAIR=1 %clangxx -asan -O0 -mllvm -asan-opt=false -mllvm -asan-max-ins-per-bb=1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clangxx -asan -O0 -mllvm -asan-instrument-reads=false -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NO-READ
// RUN: env MAIR=1 %clangxx -asan -O0 -mllvm -asan-instrument-writes=false -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NO-WRITE

int load_two_addresses(int *p, int *q) { return *p + *q; }

// CHECK-LABEL: define {{.*}} @_Z18load_two_addressesPiS_
// CHECK-COUNT-2: call void @__asan_report_load4

// NO-READ-LABEL: define {{.*}} @_Z18load_two_addressesPiS_
// NO-READ-NOT: call void @__asan_report_load4

void read_then_write(int *p) { *p = *p + 1; }

// CHECK-LABEL: define {{.*}} @_Z15read_then_writePi
// CHECK: call void @__asan_report_load4
// CHECK: call void @__asan_report_store4

// NO-READ-LABEL: define {{.*}} @_Z15read_then_writePi
// NO-READ: call void @__asan_report_store4
// NO-READ-NOT: call void @__asan_report_load4

int write_then_read(int *p) {
  *p = 1;
  return *p;
}

// CHECK-LABEL: define {{.*}} @_Z15write_then_readPi
// CHECK: call void @__asan_report_store4
// CHECK: call void @__asan_report_load4

// NO-READ-LABEL: define {{.*}} @_Z15write_then_readPi
// NO-READ: call void @__asan_report_store4
// NO-READ-NOT: call void @__asan_report_load4

// NO-WRITE-LABEL: define {{.*}} @_Z15write_then_readPi
// NO-WRITE: call void @__asan_report_load4
// NO-WRITE-NOT: call void @__asan_report_store4

void store_only(int *p) { *p = 4; }

// CHECK-LABEL: define {{.*}} @_Z10store_onlyPi
// CHECK: call void @__asan_report_store4

// NO-WRITE-LABEL: define {{.*}} @_Z10store_onlyPi
// NO-WRITE-NOT: call void @__asan_report_store4

