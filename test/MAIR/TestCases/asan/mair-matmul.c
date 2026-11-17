// C = A * B. MAIR folds the three loop levels into one observation per matrix:
// ASan checks one store range for C and one load range each for A and B, after
// the loops, and nothing per element. test/MAIR/TestCases/loop-matmul.ll has
// the MAIR graphs and the reasoning; mair-matmul-oob.c checks that an overflow
// is still reported.
//
// -fno-vectorize -fno-unroll-loops: at -O2 the k loop of @mm is unrolled with
// a remainder loop, whose accesses cannot fold into the same three ranges.
// The O2 prefix shows @mm_const, whose unrolled k loop has no remainder, still
// folds at plain -O2.
//
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O2 -fno-vectorize -fno-unroll-loops -S \
// RUN:   -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O2 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=O2

#define N 64

// The ranges are 64 * 64 * 4 = 16384 bytes: one __asan_{load,store}N each.
void mm_const(float C[N][N], const float A[N][N], const float B[N][N]) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      float s = 0;
      for (int k = 0; k < N; k++)
        s += A[i][k] * B[k][j];
      C[i][j] = s;
    }
}
// CHECK-LABEL: define {{.*}} @mm_const(
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 16384)
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       call void @__asan_storeN(i64 %{{.*}}, i64 16384)
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 16384)
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       {{^}$}}
// O2-LABEL:    define {{.*}} @mm_const(
// O2-NOT:      call void @__asan_{{load|store|report}}
// O2:          call void @__asan_loadN(i64 %{{.*}}, i64 16384)
// O2-NOT:      call void @__asan_{{load|store|report}}
// O2:          call void @__asan_storeN(i64 %{{.*}}, i64 16384)
// O2-NOT:      call void @__asan_{{load|store|report}}
// O2:          call void @__asan_loadN(i64 %{{.*}}, i64 16384)
// O2-NOT:      call void @__asan_{{load|store|report}}
// O2:          {{^}$}}

void mm(float *C, const float *A, const float *B, long n) {
  for (long i = 0; i < n; i++)
    for (long j = 0; j < n; j++) {
      float s = 0;
      for (long k = 0; k < n; k++)
        s += A[i * n + k] * B[k * n + j];
      C[i * n + j] = s;
    }
}
// The sizes are 4 * n * n, computed before the call.
// CHECK-LABEL: define {{.*}} @mm(
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 %{{.*}})
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       call void @__asan_storeN(i64 %{{.*}}, i64 %{{.*}})
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 %{{.*}})
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       {{^}$}}
