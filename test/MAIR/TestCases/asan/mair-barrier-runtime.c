// Calls that change ASan's shadow stay barriers for MAIR, even when they are
// nosync and nofree like lifetime markers. A check before such a call must
// not stand in for an access after it.
//
// RUN: env MAIR=1 %clang -asan -O1 -g %s -o %t
// RUN: %t none
// RUN: not %t scope 2>&1 | FileCheck %s --check-prefix=SCOPE
// RUN: not %t container 2>&1 | FileCheck %s --check-prefix=CONTAINER

#include <sanitizer/common_interface_defs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Inferred nosync, nofree and willreturn: not a barrier itself.
__attribute__((noinline)) static void fill(long *t) {
  t[0] = 1;
  t[1] = 2;
}

// The read after the scope repeats the read inside it (k is 1, unknown to the
// compiler, so both are checked), but lifetime.end poisons t in between.
__attribute__((noinline)) long scope(long k) {
  long *p;
  long s = 0;
  {
    long t[2];
    fill(t);
    p = t;
    s += p[k];
  }
  // SCOPE: ERROR: AddressSanitizer: stack-use-after-scope
  // SCOPE: #0 {{.*}} in scope {{.*}}mair-barrier-runtime.c:[[#@LINE+1]]
  s += p[k];
  return s;
}

// The second read repeats the first, but the annotation poisons buf[4..8)
// in between.
__attribute__((noinline)) long container(long *buf, long i) {
  long a = buf[i];
  __sanitizer_annotate_contiguous_container(buf, buf + 8, buf + 8, buf + 4);
  // CONTAINER: ERROR: AddressSanitizer: container-overflow
  // CONTAINER: #0 {{.*}} in container {{.*}}mair-barrier-runtime.c:[[#@LINE+1]]
  long b = buf[i];
  return a + b;
}

int main(int argc, char **argv) {
  long *buf = calloc(8, sizeof(long));
  long r = 0;
  if (!strcmp(argv[1], "scope"))
    r = scope(argc - 1);
  else if (!strcmp(argv[1], "container"))
    r = container(buf, argc + 3);
  free(buf);
  printf("%ld\n", r);
  return 0;
}
