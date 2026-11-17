// RUN: env MAIR=1 %clangxx -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s

int global_value;

// The size is not constant, so -O1 keeps the memcpy.
int tsan_tail(int *p, int *dst, long n) {
  int value = __atomic_load_n(p, __ATOMIC_ACQUIRE);
  __atomic_store_n(p, value, __ATOMIC_RELEASE);
  __builtin_memcpy(dst, p, n);
  global_value = value;
  return global_value;
}

// CHECK-LABEL: define {{.*}} @_Z9tsan_tailPiS_l
// CHECK: call void @__tsan_func_entry
// CHECK: call i32 @__tsan_atomic32_load
// CHECK: call void @__tsan_atomic32_store
// CHECK: call ptr @__tsan_memcpy
// CHECK: call void @__tsan_write4
// CHECK: call void @__tsan_func_exit
