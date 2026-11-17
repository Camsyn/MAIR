// __tsan_read_periodic records each trip as native TSan records that access in
// one iteration: a trip within a shadow cell, one crossing into the next, a
// 16-byte trip, a trip of any other size, and trips at a negative stride. A
// race on a trip is reported at the caller; a write to a gap is not a race.
// After many calls, the trace has switched parts and a race is still reported
// at the caller.
// RUN: %clang -tsan -O1 -g %s -o %t
// RUN: not %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
void __tsan_read_periodic(void *start, long stride, unsigned long size,
                          unsigned long count);
static unsigned barrier;

static char buf[8][128] __attribute__((aligned(64)));
static char *target;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

// Writes the byte at target, ordered before the read in time but not by
// happens-before.
static void *writer(void *arg) {
  *target = 1;
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

static void run(char *byte, void (*reader)(void), const char *name) {
  pthread_t t;
  target = byte;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  reader();
  pthread_join(t, NULL);
  fprintf(stderr, "%s done\n", name);
}

// 4 bytes at a 16-byte stride: trip 3 is buf[0][48, 52).
__attribute__((noinline)) void read_cell(void) {
  __tsan_read_periodic(buf[0], 16, 4, 8);
}
// 4 bytes from offset 6 at a 16-byte stride: each trip crosses into the next
// cell, and buf[1][9] is in the second one of trip 0.
__attribute__((noinline)) void read_crossing(void) {
  __tsan_read_periodic(buf[1] + 6, 16, 4, 4);
}
// 16 bytes from offset 4 at a 32-byte stride: trip 1 is buf[2][36, 52).
__attribute__((noinline)) void read_16(void) {
  __tsan_read_periodic(buf[2] + 4, 32, 16, 3);
}
// 12 bytes at a 16-byte stride, recorded as a range per trip: trip 2 is
// buf[3][32, 44).
__attribute__((noinline)) void read_12(void) {
  __tsan_read_periodic(buf[3], 16, 12, 4);
}
// 4 bytes from offset 60 at a stride of -8: trip 7 is buf[4][4, 8).
__attribute__((noinline)) void read_down(void) {
  __tsan_read_periodic(buf[4] + 60, -8, 4, 8);
}
// buf[5][8] is in the gap between trips 0 and 1.
__attribute__((noinline)) void read_gap(void) {
  __tsan_read_periodic(buf[5], 16, 4, 8);
}
__attribute__((noinline)) void read_after_many(void) {
  // Each call after a release stores new shadow, so it is traced.
  for (int i = 0; i < 200000; ++i) {
    pthread_mutex_lock(&mu);
    pthread_mutex_unlock(&mu);
    __tsan_read_periodic(buf[6], 16, 4, 8);
  }
}

int main(void) {
  __tsan_testonly_barrier_init(&barrier, 2);
  run(&buf[0][49], read_cell, "cell");
  // CHECK:      WARNING: ThreadSanitizer: data race
  // CHECK:      Read of size 4
  // CHECK-NEXT: #0 read_cell
  // CHECK:      cell done
  run(&buf[1][9], read_crossing, "crossing");
  // CHECK:      WARNING: ThreadSanitizer: data race
  // CHECK-NEXT: Read of size
  // CHECK-NEXT: #0 read_crossing
  // CHECK:      crossing done
  run(&buf[2][50], read_16, "16");
  // CHECK:      WARNING: ThreadSanitizer: data race
  // CHECK-NEXT: Read of size
  // CHECK-NEXT: #0 read_16
  // CHECK:      16 done
  run(&buf[3][43], read_12, "12");
  // CHECK:      WARNING: ThreadSanitizer: data race
  // CHECK-NEXT: Read of size
  // CHECK-NEXT: #0 read_12
  // CHECK:      12 done
  run(&buf[4][5], read_down, "down");
  // CHECK:      WARNING: ThreadSanitizer: data race
  // CHECK-NEXT: Read of size 4
  // CHECK-NEXT: #0 read_down
  // CHECK:      down done
  run(&buf[5][8], read_gap, "gap");
  // CHECK-NOT:  WARNING
  // CHECK:      gap done
  run(&buf[6][114], read_after_many, "many");
  // CHECK:      WARNING: ThreadSanitizer: data race
  // CHECK-NEXT: Read of size 4
  // CHECK-NEXT: #0 read_after_many
  // CHECK:      many done
  return 0;
}
