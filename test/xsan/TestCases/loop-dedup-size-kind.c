// The loop optimization drops a MOP as a duplicate of a later one at the same
// address in the same block only if the later one stands for it: it covers at
// least as many bytes, and it is a write or the dropped one is a read. A read
// does not stand for a write (TSan would miss a write race), nor a narrow write
// for a wide read (ASan would miss the bytes beyond it).
//
// clang-format off
// RUN: %clang_xsan -fno-sanitize=undefined -O2 -fno-vectorize -fno-unroll-loops -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %clang_xsan -fno-sanitize=undefined -O2 -fno-vectorize -fno-unroll-loops %s -o %t
// RUN: %run %t in 2>&1 | FileCheck %s --check-prefix=IN
// RUN: not %run %t race 2>&1 | FileCheck %s --check-prefix=RACE
// RUN: not %run %t overflow 2>&1 | FileCheck %s --check-prefix=OVERFLOW
// clang-format on

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 64
int shared[N];
int sink_q;

// The store to a[i] and the load of a[i] after it are both kept: the load does
// not stand for the store. *q may alias a[i], so the load is not forwarded.
// IR-LABEL: define {{.*}} @sum_alias(
// IR-DAG:   call void @__xsan_read_range(ptr %a,
// IR-DAG:   call void @__xsan_write_range(ptr %a,
__attribute__((noinline)) int sum_alias(int *a, int *q, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    a[i] = 1;
    *q = i;
    s += a[i];
  }
  return s;
}

// The 1-byte store to a[i] does not stand for the 4-byte load of a[i].
// IR-LABEL: define {{.*}} @narrow_write(
// IR-DAG:   call void @__xsan_period_write1(ptr %a,
// IR-DAG:   call void @__xsan_read_range(ptr %a,
__attribute__((noinline)) void narrow_write(int *a, int *out, int n) {
  for (int i = 0; i < n; i++) {
    int v = a[i];
    *(char *)&a[i] = 0;
    out[i] = v;
  }
}

// A store covers the load of the same bytes before it (a[i]++): one check.
// IR-LABEL: define {{.*}} @increment(
// IR:       call void @__xsan_write_range(ptr %a,
// IR-NOT:   call void @__xsan_read_range(
// IR-LABEL: define {{.*}} @reader(
__attribute__((noinline)) void increment(int *a, int n) {
  for (int i = 0; i < n; i++)
    a[i]++;
}

__attribute__((noinline)) int reader(int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

volatile int r_sum, m_sum;
static void *t_reader(void *arg) {
  r_sum = reader(shared, N);
  return 0;
}

int main(int argc, char **argv) {
  if (!strcmp(argv[1], "in")) {
    int *a = (int *)malloc(4 * sizeof(int));
    int out[4];
    memset(a, 0, 4 * sizeof(int));
    narrow_write(a, out, 4);
    increment(a, 4);
    m_sum = sum_alias(a, &sink_q, 4);
    free(a);
    // IN: done
    printf("done\n");
    return 0;
  }

  if (!strcmp(argv[1], "race")) {
    // No happens-before between the reader thread and the writes of sum_alias.
    pthread_t t;
    pthread_create(&t, 0, t_reader, 0);
    sleep(1);
    m_sum = sum_alias(shared, &sink_q, N);
    pthread_join(t, 0);
    // RACE: WARNING: ThreadSanitizer: data race
    // RACE-NEXT: Write of size
    return 0;
  }

  if (!strcmp(argv[1], "overflow")) {
    // The last int only has its first byte in bounds.
    int *a = (int *)malloc(4 * 4 - 3);
    int out[4];
    narrow_write(a, out, 4);
    // OVERFLOW: ERROR: AddressSanitizer: heap-buffer-overflow
    // OVERFLOW: READ of size
    // OVERFLOW: in narrow_write
    free(a);
    return 0;
  }
  return 1;
}
