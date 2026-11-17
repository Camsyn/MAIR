// Object bounds elimination is ASan only: TSan checks no bounds. TSan has no
// -mair-asan-boe option, and trusts no object, so naming boe in its pipeline
// deletes nothing.
//
// RUN: env MAIR=1 not %clang -tsan -O1 -mllvm -mair-asan-boe -c %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPTION
// RUN: env MAIR=1 %clang -tsan -O1 -mair-stats \
// RUN:   -mllvm -mair-pipeline=roe,boe -S -emit-llvm %s -o - 2>&1 \
// RUN:   | FileCheck %s

// OPTION: Unknown command line argument '-mair-asan-boe'

// TSan never records reads of constant data (NativeTsanOmits), so a
// constant table has no observation to begin with.
static const unsigned char T[256] = {1, 2, 3};

// CHECK-LABEL: define {{.*}} @lookup(
// CHECK:         call void @__tsan_read4(
// CHECK-NOT:     call void @__tsan_read
// CHECK:         ret i32
int lookup(int *p, unsigned char c) { return *p + T[c]; }

// CHECK: MAIR stats
// CHECK: roe{{ +}}in 1{{$}}
// CHECK-NOT: boe
