// RUN: env MAIR=1 %clangxx -asan -std=c++17 -S -emit-llvm %s -o - | FileCheck %s

typedef int v4i __attribute__((ext_vector_type(4)));
typedef bool v4b __attribute__((ext_vector_type(4)));

v4i f(int *p, v4b mask, v4i passthru) {
  return __builtin_masked_load(mask, p, passthru);
}

// CHECK-LABEL: define {{.*}} @_Z1f
// CHECK: call void @__asan_report_load4
// CHECK: call <4 x i32> @llvm.masked.load
