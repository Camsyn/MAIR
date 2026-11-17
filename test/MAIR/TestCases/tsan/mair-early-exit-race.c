// A loop that leaves on a data-dependent test is recorded as one TSan range
// access at its exit, sized by an induction variable there. A race with any
// one iteration must still be reported, for reads and for writes, and for a
// loop that has only pointer induction variables.
// RUN: env MAIR=1 %clang -tsan -O2 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -tsan -O2 -g %s -o %t
// RUN: not %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static long scanned[64];
static long filled[64];
static long copied[64];
static long key[64];

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) long until_neg(const long *a) {
  long i = 0;
  while (a[i] >= 0)
    i++;
  // CHECK: Read of size
  // CHECK-NEXT: #0 until_neg {{.*}}mair-early-exit-race.c:[[#@LINE-3]]
  return i;
}
// IR-LABEL: define {{.*}} @until_neg(
// IR-NOT: call void @__tsan_read8
// IR: call void @__tsan_read_range(
// IR-NOT: call void @__tsan_read8
// IR: {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) void fill_until(const long *k, long *d) {
  for (long i = 0; k[i] >= 0; i++)
    d[i] = i + 1;
  // CHECK: Write of size
  // CHECK-NEXT: #0 fill_until {{.*}}mair-early-exit-race.c:[[#@LINE-2]]
}
// IR-LABEL: define {{.*}} @fill_until(
// IR-NOT: call void @__tsan_write8
// IR: call void @__tsan_write_range(
// IR-NOT: call void @__tsan_write8
// IR: {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) void copy_until(long *d, const long *s) {
  while ((*d++ = *s++) != 0)
    ;
  // CHECK: Write of size
  // CHECK-NEXT: #0 copy_until {{.*}}mair-early-exit-race.c:[[#@LINE-3]]
}
// IR-LABEL: define {{.*}} @copy_until(
// IR-NOT: call void @__tsan_write8
// IR: call void @__tsan_write_range(
// IR-NOT: call void @__tsan_write8
// IR: {{^}$}}

__attribute__((noinline)) void write_one(long *p, long i) { p[i] = 1; }

static void *writer(void *arg) {
  // One element in the middle of each loop's footprint.
  write_one(scanned, 20);
  write_one(filled, 30);
  write_one(copied, 10);
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(void) {
  for (int i = 0; i < 64; i++)
    key[i] = scanned[i] = i + 1;
  scanned[40] = -1;
  key[50] = -1;
  long source[32];
  for (int i = 0; i < 31; i++)
    source[i] = i + 1;
  source[31] = 0;
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  long v = until_neg(scanned);
  fill_until(key, filled);
  copy_until(copied, source);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %ld\n", v);
  return 0;
}
// CHECK: DONE 40
