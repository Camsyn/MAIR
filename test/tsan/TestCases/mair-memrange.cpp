// RUN: env MAIR=1 %clangxx_tsan -O0 -S -emit-llvm %s -o - | FileCheck %s

void copy(char *dst, char *src, long n) { __builtin_memcpy(dst, src, n); }

void set(char *dst, long n) { __builtin_memset(dst, 0, n); }

// CHECK-LABEL: define {{.*}} @_Z4copyPcS_l
// CHECK: call void @__tsan_write_range
// CHECK: call void @__tsan_read_range
// CHECK-NOT: call {{.*}} @__tsan_memcpy

// CHECK-LABEL: define {{.*}} @_Z3setPcl
// CHECK: call void @__tsan_write_range
// CHECK-NOT: call {{.*}} @__tsan_memset
// CHECK-NOT: call void @__tsan_read_range
