// A merged observation must not create races the source accesses do not have:
// merged reads stay reads, and a merged access records only the bytes its
// sources touch, not the bytes next to them.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
// --check-prefix=IR RUN: env MAIR=1 %clang -tsan -O1 -g %s -o %t RUN: %t 2>&1 |
// FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

struct S {
  int x, y, z, w;
};
static struct S s = {1, 2, 3, 4};

__attribute__((noinline)) int read_pair(struct S *p) { return p->x + p->y; }
// IR-LABEL: define {{.*}} @read_pair(
// IR: call void @__tsan_{{(unaligned_)?}}read8(ptr %p)

__attribute__((noinline)) void write_zw(struct S *p) {
  p->z = 5;
  p->w = 6;
}
// IR-LABEL: define {{.*}} @write_zw(
// IR: call void @__tsan_{{(unaligned_)?}}write8(ptr %{{.*}})

static void *worker(void *arg) {
  int v = read_pair(&s);
  __tsan_testonly_barrier_wait(&barrier);
  write_zw(&s);
  __tsan_testonly_barrier_wait(&barrier);
  return (void *)(long)v;
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, worker, NULL);
  // Both threads only read x and y.
  __tsan_testonly_barrier_wait(&barrier);
  int v = read_pair(&s);
  // The other thread writes z and w right next to x and y.
  __tsan_testonly_barrier_wait(&barrier);
  v += read_pair(&s);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %d\n", v);
  return 0;
}
// CHECK-NOT: WARNING: ThreadSanitizer
// CHECK: DONE 6
