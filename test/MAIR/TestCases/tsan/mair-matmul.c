// C = A * B. MAIR folds the three loop levels into one observation per matrix:
// TSan records one write range for C and one read range each for A and B,
// and nothing per element. test/MAIR/TestCases/loop-matmul.ll has the MAIR
// graphs and the reasoning.
//
// -fno-vectorize -fno-unroll-loops: at -O2 the k loop of @mm is unrolled with
// a remainder loop, whose accesses cannot fold into the same three ranges.
// The O2 prefix shows @mm_const, whose unrolled k loop has no remainder, still
// folds at plain -O2.
//
// -ffp-contract=off: s += a * b otherwise becomes a call to llvm.fmuladd,
// which TSan still treats as a barrier that keeps accesses in the loop.
// TODO: drop the flag once nosync calls stop being barriers (WP1 of
// .tasks/new_passes_impl_20261117_1720).
//
// RUN: env MAIR=1 %clang -tsan -O1 -ffp-contract=off -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s
// RUN: env MAIR=1 %clang -tsan -O2 -fno-vectorize -fno-unroll-loops \
// RUN:   -ffp-contract=off -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -tsan -O2 -ffp-contract=off -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=O2

#define N 64

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
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       call void @__tsan_read_range(ptr %B, i64 16384)
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       call void @__tsan_write_range(ptr %C, i64 16384)
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       call void @__tsan_read_range(ptr %A, i64 16384)
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       {{^}$}}
// O2-LABEL:    define {{.*}} @mm_const(
// O2-NOT:      call void @__tsan_{{read|write|unaligned}}
// O2:          call void @__tsan_read_range(ptr %B, i64 16384)
// O2-NOT:      call void @__tsan_{{read|write|unaligned}}
// O2:          call void @__tsan_write_range(ptr %C, i64 16384)
// O2-NOT:      call void @__tsan_{{read|write|unaligned}}
// O2:          call void @__tsan_read_range(ptr %A, i64 16384)
// O2-NOT:      call void @__tsan_{{read|write|unaligned}}
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
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       call void @__tsan_read_range(ptr %B, i64 %{{.*}})
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       call void @__tsan_write_range(ptr %C, i64 %{{.*}})
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       call void @__tsan_read_range(ptr %A, i64 %{{.*}})
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       {{^}$}}
