// RUN: env MAIR=1 %clangxx_tsan -O0 -S -emit-llvm %s -o - | FileCheck %s

void copy(char *dst, char *src, long n) { __builtin_memcpy(dst, src, n); }

void set(char *dst, long n) { __builtin_memset(dst, 0, n); }

void move(char *dst, char *src, long n) { __builtin_memmove(dst, src, n); }

void copy_to_shared(char *dst) {
  char local = 123;
  __builtin_memcpy(dst, &local, 1);
}

// CHECK-LABEL: define {{.*}} @_Z4copyPcS_l
// CHECK: call ptr @__tsan_memcpy
// CHECK-NOT: call void @__tsan_read_range
// CHECK-NOT: call void @__tsan_write_range

// CHECK-LABEL: define {{.*}} @_Z3setPcl
// CHECK: call ptr @__tsan_memset
// CHECK-NOT: call void @__tsan_read_range
// CHECK-NOT: call void @__tsan_write_range

// CHECK-LABEL: define {{.*}} @_Z4movePcS_l
// CHECK: call ptr @__tsan_memmove
// CHECK-NOT: call void @__tsan_read_range
// CHECK-NOT: call void @__tsan_write_range

// CHECK-LABEL: define {{.*}} @_Z14copy_to_sharedPc
// CHECK: call ptr @__tsan_memcpy
// CHECK-NOT: call void @__tsan_read_range
// CHECK-NOT: call void @__tsan_write_range
