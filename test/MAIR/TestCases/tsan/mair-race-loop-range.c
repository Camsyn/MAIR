// A loop whose iterations cover contiguous bytes is recorded as one TSan range
// access at the loop exit. A race with any one iteration must still be
// reported, for both upward and downward loops.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
// --check-prefix=IR RUN: env MAIR=1 %clang -tsan -O1 -g %s -o %t RUN: not %t
// 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static int up[64];
static int down[64];

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i];
  // CHECK: Read of size
  // CHECK-NEXT: #0 sum {{.*}}mair-race-loop-range.c:[[#@LINE-2]]
  return s;
}
// IR-LABEL: define {{.*}} @sum(
// IR-NOT: call void @__tsan_read4
// IR: call void @__tsan_read_range(
// IR-NOT: call void @__tsan_read4
// IR: {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) void fill_down(int *p, int n) {
  for (int i = n - 1; i >= 0; --i)
    p[i] = i; // Not 0: a zero fill would become a memset.
  // CHECK: Write of size
  // CHECK-NEXT: #0 fill_down {{.*}}mair-race-loop-range.c:[[#@LINE-2]]
}
// IR-LABEL: define {{.*}} @fill_down(
// IR-NOT: call void @__tsan_write4
// IR: call void @__tsan_write_range(
// IR-NOT: call void @__tsan_write4
// IR: {{^}$}}

__attribute__((noinline)) void write_one(int *p, int i) { p[i] = 1; }

static void *writer(void *arg) {
  // One element in the middle of each range.
  write_one(up, 37);
  write_one(down, 21);
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  int v = sum(up, 64);
  fill_down(down, 64);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %d\n", v);
  return 0;
}
// CHECK: DONE 1
