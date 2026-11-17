// The range recorded at the exit of a data-dependent loop must cover exactly
// the bytes the iterations touch: not the element after the one that stopped
// the loop, not an element the loop left before writing, and nothing when it
// leaves at once. Reads stay reads.
// RUN: env MAIR=1 %clang -tsan -O2 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -tsan -O2 -g %s -o %t
// RUN: %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static long scanned[64];
static long shared[64];
static long filled[64];
static long untouched[64];
static long copied[64];
static long key[64];
static long stop[64];

__attribute__((noinline)) long until_neg(const long *a) {
  long i = 0;
  while (a[i] >= 0)
    i++;
  return i;
}
// IR-LABEL: define {{.*}} @until_neg(
// IR: call void @__tsan_read_range(

__attribute__((noinline)) void fill_until(const long *k, long *d) {
  for (long i = 0; k[i] >= 0; i++)
    d[i] = i + 1;
}
// IR-LABEL: define {{.*}} @fill_until(
// IR: call void @__tsan_write_range(

__attribute__((noinline)) void copy_until(long *d, const long *s) {
  while ((*d++ = *s++) != 0)
    ;
}
// IR-LABEL: define {{.*}} @copy_until(
// IR: call void @__tsan_write_range(

__attribute__((noinline)) void write_one(long *p, long i) { p[i] = 1; }

static void *writer(void *arg) {
  // Right after the element that stops the scan, the first element the fill
  // does not write, an array whose fill stops at once, and right after the
  // copy.
  write_one(scanned, 41);
  write_one(filled, 50);
  write_one(untouched, 0);
  write_one(copied, 32);
  long v = until_neg(shared);
  __tsan_testonly_barrier_wait(&barrier);
  return (void *)v;
}

int main(void) {
  for (int i = 0; i < 64; i++)
    key[i] = scanned[i] = shared[i] = i + 1;
  scanned[40] = -1;
  shared[60] = -1;
  key[50] = -1;
  stop[0] = -1;
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
  fill_until(stop, untouched);
  copy_until(copied, source);
  // Both threads only read shared.
  v += until_neg(shared);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %ld\n", v);
  return 0;
}
// CHECK-NOT: WARNING: ThreadSanitizer
// CHECK: DONE 100
