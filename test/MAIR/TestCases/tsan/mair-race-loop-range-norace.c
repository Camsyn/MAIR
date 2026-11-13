// A loop range must cover exactly the bytes the iterations touch: not the
// element after an upward loop, not the element before a downward loop, and
// nothing at all when the loop runs zero times. Reads stay reads.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
// --check-prefix=IR RUN: env MAIR=1 %clang -tsan -O1 -g %s -o %t RUN: %t 2>&1 |
// FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static int up[64];
static int down[64];
static int shared[64];

__attribute__((noinline)) int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i];
  return s;
}
// IR-LABEL: define {{.*}} @sum(
// IR: call void @__tsan_read_range(

__attribute__((noinline)) void fill_down(int *p, int n) {
  for (int i = n - 1; i >= 0; --i)
    p[i] = i; // Not 0: a zero fill would become a memset.
}
// IR-LABEL: define {{.*}} @fill_down(
// IR: call void @__tsan_write_range(

__attribute__((noinline)) void write_one(int *p, int i) { p[i] = 1; }

static void *writer(void *arg) {
  // Right after up[0, 33), right before down[33, 64), and in a loop that runs
  // zero times. The first two share an 8-byte shadow cell with an element the
  // other thread touches.
  write_one(up, 33);
  write_one(down, 32);
  write_one(up, 40);
  int v = sum(shared, 64);
  __tsan_testonly_barrier_wait(&barrier);
  return (void *)(long)v;
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  int v = sum(up, 33);
  fill_down(down + 33, 31);
  v += sum(up + 40, 0);
  // Both threads only read shared.
  v += sum(shared, 64);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %d\n", v);
  return 0;
}
// CHECK-NOT: WARNING: ThreadSanitizer
// CHECK: DONE 0
