// A data-dependent loop whose only integer counter is an unsigned char wraps
// it after 256 iterations; built with -fno-strict-overflow, its pointer
// induction variables carry no wrap flags either. MAIR records one TSan range
// access at the exit, sized by a 64-bit counter it inserts into the loop
// (docs/mair-loops.md, 数据相关的出口). The loops run 300 times; a race with
// an iteration past the 256th must still be reported, for reads and writes.
// RUN: env MAIR=1 %clang -tsan -O2 -fno-strict-overflow -S -emit-llvm %s \
// RUN:   -o - | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -tsan -O2 -g -fno-strict-overflow %s -o %t
// RUN: not %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static long scanned[400];
static long filled[400];
static long key[400];

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) unsigned char scan_u8(const long *a) {
  unsigned char n = 0;
  while (*a >= 0) {
    a++;
    n++;
  }
  // CHECK: Read of size
  // CHECK-NEXT: #0 scan_u8 {{.*}}mair-exit-counter-race.c:[[#@LINE-5]]
  return n;
}
// IR-LABEL: define {{.*}} @scan_u8(
// IR: %mair.count = phi i64
// IR-NOT: call void @__tsan_read8
// IR: call void @__tsan_read_range(
// IR-NOT: call void @__tsan_read8
// IR: {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) unsigned char fill_u8(const long *k, long *d) {
  unsigned char n = 0;
  while (*k++ >= 0)
    *d++ = n++;
  // CHECK: Write of size
  // CHECK-NEXT: #0 fill_u8 {{.*}}mair-exit-counter-race.c:[[#@LINE-2]]
  return n;
}
// IR-LABEL: define {{.*}} @fill_u8(
// IR: %mair.count = phi i64
// IR-NOT: call void @__tsan_write8
// IR: call void @__tsan_write_range(
// IR-NOT: call void @__tsan_write8
// IR: {{^}$}}

__attribute__((noinline)) void write_one(long *p, long i) { p[i] = 1; }

static void *writer(void *arg) {
  // Elements past the 256th: a count taken modulo 256 (44) misses them.
  write_one(scanned, 290);
  write_one(filled, 280);
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(void) {
  for (int i = 0; i < 400; i++)
    key[i] = scanned[i] = i + 1;
  scanned[300] = -1;
  key[300] = -1;
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  unsigned v = scan_u8(scanned);
  v += fill_u8(key, filled);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %u\n", v);
  return 0;
}
// 300 iterations each: n wraps to 300 - 256 = 44.
// CHECK: DONE 88
