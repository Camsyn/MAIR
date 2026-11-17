// The inline fast path of a merged constant-size range must report every
// overflow the runtime call reported, and nothing in bounds: one byte past
// either end, a poisoned granule in the middle, and a granule poisoned only
// in part. When it fails, __asan_loadN/storeN reports as it did before: the
// merged size, the location of the access that runs first, and the first
// poisoned byte.
//
// RUN: env MAIR=1 %clang -asan -O1 -g -fno-slp-vectorize %s -o %t
// RUN: %t in
// RUN: not %t r24-end 2>&1 | FileCheck %s --check-prefix=R24-END
// RUN: not %t w24-end 2>&1 | FileCheck %s --check-prefix=W24-END
// RUN: not %t r24-begin 2>&1 | FileCheck %s --check-prefix=R24-BEGIN
// RUN: not %t r24-mid 2>&1 | FileCheck %s --check-prefix=R24-MID
// RUN: not %t r20-end 2>&1 | FileCheck %s --check-prefix=R20-END
// RUN: not %t w20-end 2>&1 | FileCheck %s --check-prefix=W20-END
// RUN: not %t w20u-end 2>&1 | FileCheck %s --check-prefix=W20U-END
// RUN: not %t r20u-begin 2>&1 | FileCheck %s --check-prefix=R20U-BEGIN
// RUN: not %t w20u-begin 2>&1 | FileCheck %s --check-prefix=W20U-BEGIN
// RUN: not %t w20u-mid 2>&1 | FileCheck %s --check-prefix=W20U-MID
// RUN: not %t r40-end 2>&1 | FileCheck %s --check-prefix=R40-END
// RUN: not %t w64-end 2>&1 | FileCheck %s --check-prefix=W64-END
// RUN: not %t r64-mid 2>&1 | FileCheck %s --check-prefix=R64-MID
// RUN: not %t r64-part 2>&1 | FileCheck %s --check-prefix=R64-PART
// RUN: not %t r128-end 2>&1 | FileCheck %s --check-prefix=R128-END
// RUN: not %t r128-mid 2>&1 | FileCheck %s --check-prefix=R128-MID
// RUN: env MAIR=1 %clang -asan -O1 -fno-slp-vectorize -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR

#include <sanitizer/asan_interface.h>
#include <stdlib.h>
#include <string.h>

struct s24 {
  long a, b, c;
};
struct s20 {
  long a, b;
  int c;
};
struct s40 {
  long f[5];
};
struct s64 {
  long f[8];
};
struct s128 {
  long f[16];
};

__attribute__((noinline)) long load24(struct s24 *s) {
  return s->a + s->b + s->c;
}
__attribute__((noinline)) void store24(struct s24 *s) {
  s->a = 1;
  s->b = 2;
  s->c = 3;
}
// The partial granule at the end: 20 bytes from an aligned start.
__attribute__((noinline)) long load20(struct s20 *s) {
  return s->a + s->b + s->c;
}
__attribute__((noinline)) void store20(struct s20 *s) {
  s->a = 1;
  s->b = 2;
  s->c = 3;
}
// From a 4-byte aligned start the shadow offsets are computed at run time.
__attribute__((noinline)) void store_row(int *p) {
  p[0] = 1;
  p[1] = 2;
  p[2] = 3;
  p[3] = 4;
  p[4] = 5;
}
__attribute__((noinline)) long load_row(int *p) {
  return (long)p[0] + p[1] + p[2] + p[3] + p[4];
}
__attribute__((noinline)) long load40(struct s40 *s) {
  long t = 0;
  for (int i = 0; i < 5; ++i)
    t += s->f[i];
  return t;
}
__attribute__((noinline)) void store64(struct s64 *s) {
  for (int i = 0; i < 8; ++i)
    s->f[i] = i;
}
__attribute__((noinline)) long load64(struct s64 *s) {
  long t = 0;
  for (int i = 0; i < 8; ++i)
    t += s->f[i];
  return t;
}
__attribute__((noinline)) long load128(struct s128 *s) {
  long t = 0;
  for (int i = 0; i < 16; ++i)
    t += s->f[i];
  return t;
}

volatile long sink;

static int is(const char *mode, const char *name) { return !strcmp(mode, name); }

int main(int argc, char **argv) {
  const char *m = argv[1];
  long r = 0;
  // Each object is allocated with exactly the bytes its range needs, or one
  // byte less; malloc aligns it to 8 bytes or more.
  char *b24 = (char *)malloc(is(m, "r24-end") || is(m, "w24-end") ? 23 : 24);
  char *b20 = (char *)malloc(is(m, "r20-end") || is(m, "w20-end") ? 19 : 20);
  // Room for 20 bytes from offset 4, one byte less for w20u-end.
  char *b20u = (char *)malloc(is(m, "w20u-end") ? 23 : 24);
  char *b40 = (char *)malloc(is(m, "r40-end") ? 39 : 40);
  char *b64 = (char *)malloc(is(m, "w64-end") ? 63 : 64);
  char *b128 = (char *)malloc(is(m, "r128-end") ? 127 : 128);
  memset(b24, 0, 23);
  memset(b20, 0, 19);
  memset(b40, 0, 39);
  memset(b64, 0, 63);
  memset(b128, 0, 127);

  if (is(m, "in")) {
    store24((struct s24 *)b24);
    r += load24((struct s24 *)b24);
    store20((struct s20 *)b20);
    r += load20((struct s20 *)b20);
    store_row((int *)(b20u + 4));
    r += load40((struct s40 *)b40);
    store64((struct s64 *)b64);
    r += load64((struct s64 *)b64);
    r += load128((struct s128 *)b128);
    // An odd start: 20 bytes from offset 3.
    store_row((int *)(b20u + 3));
    r += load_row((int *)(b20u + 1));
    sink = r;
    free(b128);
    free(b64);
    free(b40);
    free(b20u);
    free(b20);
    free(b24);
    return 0;
  }

  // R24-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // R24-END: READ of size 24
  // R24-END: #0 {{.*}} in load24 {{.*}}mair-inline-range-runtime.c
  // R24-END: is located 0 bytes after 23-byte region
  if (is(m, "r24-end"))
    r = load24((struct s24 *)b24);
  // W24-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // W24-END: WRITE of size 24
  // W24-END: #0 {{.*}} in store24 {{.*}}mair-inline-range-runtime.c
  // W24-END: is located 0 bytes after 23-byte region
  if (is(m, "w24-end"))
    store24((struct s24 *)b24);
  // An aligned range can only start a whole granule early.
  // R24-BEGIN: ERROR: AddressSanitizer: heap-buffer-overflow
  // R24-BEGIN: READ of size 24
  // R24-BEGIN: is located 8 bytes before 24-byte region
  if (is(m, "r24-begin"))
    r = load24((struct s24 *)(b24 - 8));
  // R24-MID: ERROR: AddressSanitizer: use-after-poison
  // R24-MID: READ of size 24
  // R24-MID: #0 {{.*}} in load24 {{.*}}mair-inline-range-runtime.c
  if (is(m, "r24-mid")) {
    ASAN_POISON_MEMORY_REGION(b24 + 8, 8);
    r = load24((struct s24 *)b24);
  }

  // R20-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // R20-END: READ of size 20
  // R20-END: #0 {{.*}} in load20 {{.*}}mair-inline-range-runtime.c
  // R20-END: is located 0 bytes after 19-byte region
  if (is(m, "r20-end"))
    r = load20((struct s20 *)b20);
  // W20-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // W20-END: WRITE of size 20
  // W20-END: is located 0 bytes after 19-byte region
  if (is(m, "w20-end"))
    store20((struct s20 *)b20);

  // W20U-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // W20U-END: WRITE of size 20
  // W20U-END: #0 {{.*}} in store_row {{.*}}mair-inline-range-runtime.c
  // W20U-END: is located 0 bytes after 23-byte region
  if (is(m, "w20u-end"))
    store_row((int *)(b20u + 4));
  // R20U-BEGIN: ERROR: AddressSanitizer: heap-buffer-overflow
  // R20U-BEGIN: READ of size 20
  // R20U-BEGIN: #0 {{.*}} in load_row {{.*}}mair-inline-range-runtime.c
  // R20U-BEGIN: is located 1 bytes before 24-byte region
  if (is(m, "r20u-begin"))
    r = load_row((int *)(b20u - 1));
  // A write before the start would hit the heap chunk header if a store ran
  // before the check; poison the first granule of the object instead, and
  // start the range on its last byte.
  // W20U-BEGIN: ERROR: AddressSanitizer: use-after-poison
  // W20U-BEGIN: WRITE of size 20
  // W20U-BEGIN: #0 {{.*}} in store_row {{.*}}mair-inline-range-runtime.c
  if (is(m, "w20u-begin")) {
    ASAN_POISON_MEMORY_REGION(b40, 8);
    store_row((int *)(b40 + 7));
  }
  // W20U-MID: ERROR: AddressSanitizer: use-after-poison
  // W20U-MID: WRITE of size 20
  // W20U-MID: #0 {{.*}} in store_row {{.*}}mair-inline-range-runtime.c
  if (is(m, "w20u-mid")) {
    ASAN_POISON_MEMORY_REGION(b20u + 8, 8);
    store_row((int *)(b20u + 4));
  }

  // R40-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // R40-END: READ of size 40
  // R40-END: is located 0 bytes after 39-byte region
  if (is(m, "r40-end"))
    r = load40((struct s40 *)b40);
  // W64-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // W64-END: WRITE of size 64
  // W64-END: is located 0 bytes after 63-byte region
  if (is(m, "w64-end"))
    store64((struct s64 *)b64);
  // R64-MID: ERROR: AddressSanitizer: use-after-poison
  // R64-MID: READ of size 64
  // R64-MID: #0 {{.*}} in load64 {{.*}}mair-inline-range-runtime.c
  if (is(m, "r64-mid")) {
    ASAN_POISON_MEMORY_REGION(b64 + 24, 8);
    r = load64((struct s64 *)b64);
  }
  // Poisoning the last 4 bytes of a granule leaves its shadow at 4: only its
  // first 4 bytes stay addressable. The runtime calls a partial granule an
  // unknown-crash.
  // R64-PART: ERROR: AddressSanitizer: unknown-crash
  // R64-PART: READ of size 64
  // R64-PART: #0 {{.*}} in load64 {{.*}}mair-inline-range-runtime.c
  if (is(m, "r64-part")) {
    ASAN_POISON_MEMORY_REGION(b64 + 36, 4);
    r = load64((struct s64 *)b64);
  }
  // R128-END: ERROR: AddressSanitizer: heap-buffer-overflow
  // R128-END: READ of size 128
  // R128-END: is located 0 bytes after 127-byte region
  if (is(m, "r128-end"))
    r = load128((struct s128 *)b128);
  // R128-MID: ERROR: AddressSanitizer: use-after-poison
  // R128-MID: READ of size 128
  // R128-MID: #0 {{.*}} in load128 {{.*}}mair-inline-range-runtime.c
  if (is(m, "r128-mid")) {
    ASAN_POISON_MEMORY_REGION(b128 + 64, 8);
    r = load128((struct s128 *)b128);
  }
  return (int)r;
}

// Every function above checks its range on the inline fast path.
// IR-LABEL: define {{.*}} @load24(
// IR:       call void @__asan_loadN(i64 %{{.*}}, i64 24)
// IR-LABEL: define {{.*}} @store24(
// IR:       call void @__asan_storeN(i64 %{{.*}}, i64 24)
// IR-LABEL: define {{.*}} @load20(
// IR:       load i8
// IR:       call void @__asan_loadN(i64 %{{.*}}, i64 20)
// IR-LABEL: define {{.*}} @store20(
// IR:       load i8
// IR:       call void @__asan_storeN(i64 %{{.*}}, i64 20)
// IR-LABEL: define {{.*}} @store_row(
// IR:       and i64 %{{.*}}, 7
// IR:       call void @__asan_storeN(i64 %{{.*}}, i64 20)
// IR-LABEL: define {{.*}} @load_row(
// IR:       and i64 %{{.*}}, 7
// IR:       call void @__asan_loadN(i64 %{{.*}}, i64 20)
// IR-LABEL: define {{.*}} @load40(
// IR:       call void @__asan_loadN(i64 %{{.*}}, i64 40)
// IR-LABEL: define {{.*}} @store64(
// IR:       load i64
// IR:       call void @__asan_storeN(i64 %{{.*}}, i64 64)
// IR-LABEL: define {{.*}} @load64(
// IR:       load i64
// IR:       call void @__asan_loadN(i64 %{{.*}}, i64 64)
// IR-LABEL: define {{.*}} @load128(
// IR:       load i64
// IR:       load i64
// IR:       call void @__asan_loadN(i64 %{{.*}}, i64 128)
// IR-LABEL: define {{.*}} @main(
