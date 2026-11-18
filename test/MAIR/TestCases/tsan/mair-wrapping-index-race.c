// A loop whose index is an unsigned char wraps after 256 iterations and
// revisits a[0..]. MAIR records one TSan range access at the exit, the
// min(T, 256) elements from a (docs/mair-loops.md, 窄下标回环访问). The loops
// run 300 times. Another thread writes an element the loop touched: the race
// must be reported, for the read and for the write loop. It also writes the
// elements just past the 256 the loop touched: the range must not reach them
// (a summary of all 300 iterations would), so the norace run stays quiet.
// sum_u8 and fill_u8 keep one access per iteration; fill_unrolled is unrolled
// 4 times by -O2 and sum_down counts down from 255, see
// asan/mair-wrapping-index-oob.c.
//
// RUN: env MAIR=1 %clang -tsan -O2 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clang -tsan -O2 -g %s -o %t
// RUN: not %t race 2>&1 | FileCheck %s
// RUN: %t norace 2>&1 | FileCheck %s --check-prefix=NORACE

#include <pthread.h>
#include <stdio.h>
#include <string.h>

void __tsan_testonly_barrier_init(unsigned *barrier, unsigned count);
void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

static unsigned char bytes[300];
static int ints[300];
static unsigned char downs[300];
static int unrolled[300];
static int race;

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) unsigned sum_u8(const unsigned char *a, int n) {
  unsigned s = 0;
  unsigned char i = 0;
#pragma clang loop unroll(disable) vectorize(disable)
  for (int k = 0; k < n; k++, i++)
    s += a[i];
  // CHECK: Read of size
  // CHECK-NEXT: #0 sum_u8 {{.*}}mair-wrapping-index-race.c:[[#@LINE-2]]
  return s;
}
// IR-LABEL: define {{.*}} @sum_u8(
// IR-NOT: call void @__tsan_read1
// IR: call void @__tsan_read_range(
// IR-NOT: call void @__tsan_read1
// IR: {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) void fill_u8(int *a, int n) {
  unsigned char i = 0;
#pragma clang loop unroll(disable) vectorize(disable)
  for (int k = 0; k < n; k++, i++)
    a[i] = k;
  // CHECK: Write of size
  // CHECK-NEXT: #0 fill_u8 {{.*}}mair-wrapping-index-race.c:[[#@LINE-2]]
}
// IR-LABEL: define {{.*}} @fill_u8(
// IR-NOT: call void @__tsan_write4
// IR: call void @__tsan_write_range(
// IR-NOT: call void @__tsan_write4
// IR: {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) unsigned sum_down(const unsigned char *a, int n) {
  unsigned s = 0;
  unsigned char i = 255;
#pragma clang loop unroll(disable) vectorize(disable)
  for (int k = 0; k < n; k++, i--)
    s += a[i];
  // CHECK: Read of size
  // CHECK-NEXT: #0 sum_down {{.*}}mair-wrapping-index-race.c:[[#@LINE-2]]
  return s;
}
// A downward periodic region of stride -1 and size 1 is one range.
// IR-LABEL: define {{.*}} @sum_down(
// IR-NOT: call void @__tsan_read1
// IR: call void @__tsan_read_range(
// IR-NOT: call void @__tsan_read1
// IR: {{^}$}}

// CHECK: WARNING: ThreadSanitizer: data race
__attribute__((noinline)) void fill_unrolled(int *a, int n) {
  unsigned char i = 0;
#pragma clang loop vectorize(disable)
  for (int k = 0; k < n; k++, i++)
    a[i] = k;
  // CHECK: Write of size
  // CHECK-NEXT: #0 fill_unrolled {{.*}}mair-wrapping-index-race.c:[[#@LINE-2]]
}
// The four copies of the unrolled loop are one range at its exit.
// IR-LABEL: define {{.*}} @fill_unrolled(
// IR: call void @__tsan_write_range(
// IR: {{^}$}}

__attribute__((noinline)) void write_byte(unsigned char *p, long i) {
  p[i] = 1;
}
__attribute__((noinline)) void write_int(int *p, long i) { p[i] = 1; }

static void *writer(void *arg) {
  if (race) {
    // Element 200 is read and written by the loops.
    write_byte(bytes, 200);
    write_int(ints, 200);
    write_byte(downs, 200);
    write_int(unrolled, 200);
  } else {
    // Elements 256 and up are never touched: i wraps to 0 first.
    for (long i = 256; i < 300; i++) {
      write_byte(bytes, i);
      write_int(ints, i);
      write_byte(downs, i);
      write_int(unrolled, i);
    }
  }
  __tsan_testonly_barrier_wait(&barrier);
  return NULL;
}

int main(int argc, char **argv) {
  race = argc > 1 && !strcmp(argv[1], "race");
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);
  __tsan_testonly_barrier_wait(&barrier);
  unsigned v = sum_u8(bytes, 300);
  fill_u8(ints, 300);
  v += sum_down(downs, 300);
  fill_unrolled(unrolled, 300);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE %u\n", v + ints[0] + unrolled[0]);
  return 0;
}
// CHECK: DONE
// NORACE-NOT: ThreadSanitizer
// NORACE: DONE 512
// NORACE-NOT: ThreadSanitizer
