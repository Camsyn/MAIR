// C = A * B while another thread writes one element of A. MAIR records all of
// A with one read range after the loops (mair-matmul.c); the race with that
// one element must still be reported, for the constant and the symbolic size.
//
// RUN: env MAIR=1 %clang -tsan -O2 -fno-vectorize -fno-unroll-loops \
// RUN:   -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -tsan -O2 -fno-vectorize -fno-unroll-loops \
// RUN:   -g %s -o %t
// RUN: not %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

#define N 16

static float a1[N][N], b1[N][N], c1[N][N];
static float a2[N * N], b2[N * N], c2[N * N];

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) void mm_const(float C[N][N], const float A[N][N],
                                        const float B[N][N]) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      float s = 0;
      for (int k = 0; k < N; k++)
        s += A[i][k] * B[k][j];
      // CHECK: Read of size
      // CHECK-NEXT: #0 mm_const {{.*}}mair-race-matmul.c:[[#@LINE-2]]
      C[i][j] = s;
    }
}
// IR-LABEL: define {{.*}} @mm_const(
// IR-NOT:   call void @__tsan_read4(
// IR:       call void @__tsan_read_range(ptr %A, i64 1024)
// IR-NOT:   call void @__tsan_read4(
// IR:       {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) void mm(float *C, const float *A, const float *B,
                                  long n) {
  for (long i = 0; i < n; i++)
    for (long j = 0; j < n; j++) {
      float s = 0;
      for (long k = 0; k < n; k++)
        s += A[i * n + k] * B[k * n + j];
      // CHECK: Read of size
      // CHECK-NEXT: #0 mm {{.*}}mair-race-matmul.c:[[#@LINE-2]]
      C[i * n + j] = s;
    }
}
// IR-LABEL: define {{.*}} @mm(
// IR-NOT:   call void @__tsan_read4(
// IR:       call void @__tsan_read_range(ptr %A,
// IR-NOT:   call void @__tsan_read4(
// IR:       {{^}$}}

__attribute__((noinline)) void write_one(float *p, int i) { p[i] = 2.0f; }

static void *writer(void *arg) {
  // One element in the middle of each A.
  write_one(&a1[0][0], 7 * N + 9);
  write_one(a2, 5 * N + 3);
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  mm_const(c1, a1, b1);
  mm(c2, a2, b2, N);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %f %f\n", c1[0][0], c2[0]);
  return 0;
}
// CHECK: DONE
