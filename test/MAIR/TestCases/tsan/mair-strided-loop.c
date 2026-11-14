// A loop access with gaps between its iterations, such as a[2 * i], becomes one
// __tsan_{read,write}_periodic call that records each iteration's bytes and
// none of the gaps. A race with one iteration is still reported; a write to a
// gap only is not a race.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -tsan -O1 -g %s -o %t
// RUN: not %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static int a[64];

// Reads a[0], a[2], ..., a[2 * (n - 1)].
__attribute__((noinline)) int sum_even(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[2 * i];
  return s;
}
// IR-LABEL: define {{.*}} @sum_even(
// IR: call void @__tsan_read_periodic(
// IR-NOT: call void @__tsan_read4(
// IR: ret i32

__attribute__((noinline)) void write_one(int *p, int i) { p[i] = i; }

static void *writer(void *arg) {
  // a[17] is a gap: no race.
  write_one(a, 17);
  __tsan_testonly_barrier_wait(&barrier);
  __tsan_testonly_barrier_wait(&barrier);
  // a[20] is read by iteration 10: a race.
  write_one(a, 20);
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  int v = sum_even(a, 32);
  fprintf(stderr, "GAP DONE %d\n", v);
  __tsan_testonly_barrier_wait(&barrier);
  __tsan_testonly_barrier_wait(&barrier);
  v += sum_even(a, 32);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %d\n", v);
  return 0;
}
// CHECK-NOT: WARNING: ThreadSanitizer
// CHECK: GAP DONE 0
// The periodic access is recorded at the loop exit, before the writer runs,
// so the write is the one that finds the race.
// CHECK: WARNING: ThreadSanitizer: data race
// CHECK: Write of size 4
// CHECK-NEXT: #0 write_one
// CHECK: Previous read of size 4
// CHECK-NEXT: #0 sum_even {{.*}}mair-strided-loop.c:23
// CHECK: DONE 20
// CHECK: ThreadSanitizer: reported 1 warnings
