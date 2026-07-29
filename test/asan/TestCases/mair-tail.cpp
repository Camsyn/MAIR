// RUN: env MAIR=1 %clangxx -asan -O0 -mllvm -asan-detect-invalid-pointer-pair -S -emit-llvm %s -o - | FileCheck %s

extern void stop_now() __attribute__((noreturn));
extern void use_buffer(char *);

long asan_tail(char *a, char *b) {
  char local[32];
  use_buffer(local);
  local[0] = *a;
  if (a < b)
    stop_now();
  return local[0] + (b - a);
}

// CHECK-LABEL: define {{.*}} @_Z9asan_tailPcS_
// CHECK: call i64 @__asan_stack_malloc_1
// CHECK: call void @__asan_report_load1
// CHECK: call void @__sanitizer_ptr_cmp
// CHECK: call void @__asan_handle_no_return
// CHECK: call void @__sanitizer_ptr_sub
