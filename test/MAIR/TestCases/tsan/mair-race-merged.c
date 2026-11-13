// A merged observation is recorded as one TSan access of all of its bytes. A
// race on any one of the merged fields must still be reported, at the source
// line of the merged accesses.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
// --check-prefix=IR RUN: env MAIR=1 %clang -tsan -O1 -g %s -o %t RUN: not %t
// 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

// The TSan-invisible barrier of test/tsan/TestCases/test.h: it orders the two
// threads in time without creating a happens-before edge.
void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

struct S {
  int x, y;
  long z;
};
static struct S s;

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) int read_pair(struct S *p) {
  return p->x + p->y;
  // The racing write is to y, but the merged read starts at x.
  // CHECK: Read of size 8
  // CHECK-NEXT: #0 read_pair {{.*}}mair-race-merged.c:[[#@LINE-3]]
}
// The test is only meaningful if the two reads are one access.
// IR-LABEL: define {{.*}} @read_pair(
// IR-NOT: call void @__tsan_read4
// IR: call void @__tsan_read8(ptr %p)
// IR-NOT: call void @__tsan_read4
// IR: {{^}$}}

__attribute__((noinline)) void write_y(struct S *p) {
  p->y = 1;
  // CHECK: Previous write of size 4
  // CHECK-NEXT: #0 write_y {{.*}}mair-race-merged.c:[[#@LINE-2]]
}

static void *writer(void *arg) {
  write_y(&s);
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  int v = read_pair(&s);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %d\n", v);
  return 0;
}
// CHECK: DONE 1
