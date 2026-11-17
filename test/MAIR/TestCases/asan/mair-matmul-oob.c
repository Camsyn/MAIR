// C = A * B with C one element too short. MAIR checks all of C with one store
// range after the loops (mair-matmul.c), which must still report the write of
// the last element. In-bounds matrices must not report.
//
// RUN: env MAIR=1 %clang -asan -O2 -fno-vectorize -fno-unroll-loops -g %s -o %t
// RUN: %t const-in
// RUN: %t sym-in
// RUN: not %t const-oob 2>&1 | FileCheck %s --check-prefix=CONST
// RUN: not %t sym-oob 2>&1 | FileCheck %s --check-prefix=SYM
// RUN: env MAIR=1 %clang -asan -O2 -fno-vectorize -fno-unroll-loops -S \
// RUN:   -emit-llvm %s -o - | FileCheck %s --check-prefix=IR

#include <stdlib.h>
#include <string.h>

#define N 16

__attribute__((noinline)) void mm_const(float C[N][N], const float A[N][N],
                                        const float B[N][N]) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      float s = 0;
      for (int k = 0; k < N; k++)
        s += A[i][k] * B[k][j];
      C[i][j] = s;
    }
}
// The report shows the whole range and the location of the store.
// CONST: ERROR: AddressSanitizer: heap-buffer-overflow
// CONST: WRITE of size 1024
// CONST: #0 {{.*}} in mm_const {{.*}}mair-matmul-oob.c:[[@LINE-6]]
// IR-LABEL: define {{.*}} @mm_const(
// IR-NOT:   call void @__asan_report_store4(
// IR:       call void @__asan_storeN(i64 %{{.*}}, i64 1024)
// IR-NOT:   call void @__asan_report_store4(
// IR:       {{^}$}}

__attribute__((noinline)) void mm(float *C, const float *A, const float *B,
                                  long n) {
  for (long i = 0; i < n; i++)
    for (long j = 0; j < n; j++) {
      float s = 0;
      for (long k = 0; k < n; k++)
        s += A[i * n + k] * B[k * n + j];
      C[i * n + j] = s;
    }
}
// SYM: ERROR: AddressSanitizer: heap-buffer-overflow
// SYM: WRITE of size 1024
// SYM: #0 {{.*}} in mm {{.*}}mair-matmul-oob.c:[[@LINE-5]]
// IR-LABEL: define {{.*}} @mm(
// IR-NOT:   call void @__asan_report_store4(
// IR:       call void @__asan_storeN(
// IR-NOT:   call void @__asan_report_store4(
// IR:       {{^}$}}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  const char *mode = argv[1];
  size_t elems = N * N;
  float *a = malloc(elems * sizeof(float));
  float *b = malloc(elems * sizeof(float));
  for (size_t e = 0; e < elems; ++e)
    a[e] = b[e] = 1.0f;
  // One element short: C[N - 1][N - 1] lies in the heap redzone.
  int oob = strstr(mode, "-oob") != NULL;
  float *c = malloc((elems - oob) * sizeof(float));
  if (!strncmp(mode, "const", 5))
    mm_const((float(*)[N])c, (const float(*)[N])a, (const float(*)[N])b);
  else
    mm(c, a, b, N);
  int r = c[0] != (float)N;
  free(c);
  free(b);
  free(a);
  return r;
}
