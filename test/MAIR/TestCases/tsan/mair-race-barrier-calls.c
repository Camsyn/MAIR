// The llvm.smax call in the loop is nosync and nofree, so it is no barrier:
// the loop is recorded as one range access at its exit. A race with any one
// iteration must still be reported.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -tsan -O1 -g %s -o %t
// RUN: not %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static long arr[64];

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) long maxof(long *p, long n) {
  long m = 0;
  for (long i = 0; i < n; i++)
    m = __builtin_elementwise_max(m, p[i]);
  // CHECK: Read of size
  // CHECK-NEXT: #0 maxof {{.*}}mair-race-barrier-calls.c:[[#@LINE-2]]
  return m;
}
// IR-LABEL: define {{.*}} @maxof(
// IR-NOT: call void @__tsan_read8
// IR: call void @__tsan_read_range(
// IR-NOT: call void @__tsan_read8
// IR: {{^}$}}

__attribute__((noinline)) void write_one(long *p, int i) { p[i] = 1; }

static void *writer(void *arg) {
  write_one(arr, 37);
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  long v = maxof(arr, 64);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %ld\n", v);
  return 0;
}
// CHECK: DONE 1
