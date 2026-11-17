// A call that can neither synchronize nor free memory (nosync and nofree) does
// not change ASan's shadow, and neither do the SanitizerCoverage callbacks.
// Such calls are not barriers, so a loop that makes one is still checked once
// after its exit. Calls ASan turns into shadow updates stay barriers even
// though they are nosync and nofree: lifetime markers (use-after-scope) and
// stackrestore (__asan_allocas_unpoison). So does any other call.
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O1 -fsanitize=fuzzer-no-link -S -emit-llvm %s \
// RUN:   -o - | FileCheck %s --check-prefix=FUZZ

long assume_loop(long *p, long n) {
  long s = 0;
  for (long i = 0; i < n; i++) {
    long v = p[i];
    __builtin_assume(v != 5);
    s += v;
  }
  return s;
}
// CHECK-LABEL: define {{.*}} @assume_loop(
// CHECK-NOT: call void @__asan_report_load8
// CHECK: call void @__asan_loadN(
// CHECK-NOT: call void @__asan_report_load8
// CHECK: {{^}$}}

long prefetch_loop(long *p, long n) {
  long s = 0;
  for (long i = 0; i < n; i++) {
    __builtin_prefetch(&p[i + 64]);
    s += p[i];
  }
  return s;
}
// CHECK-LABEL: define {{.*}} @prefetch_loop(
// CHECK-NOT: call void @__asan_report_load8
// CHECK: call void @__asan_loadN(
// CHECK-NOT: call void @__asan_report_load8
// CHECK: {{^}$}}

// bump is inferred nosync, nofree and willreturn.
__attribute__((noinline)) static void bump(long *c) { *c += 1; }
long nosync_call_loop(long *p, long n, long *c) {
  long s = 0;
  for (long i = 0; i < n; i++) {
    s += p[i];
    bump(c);
  }
  return s;
}
// CHECK-LABEL: define {{.*}} @nosync_call_loop(
// CHECK-NOT: call void @__asan_report_load8
// CHECK: call void @__asan_loadN(
// CHECK-NOT: call void @__asan_report_load8
// CHECK: {{^}$}}

// t lives for one iteration: ASan poisons it at each lifetime end, so the
// loop's other accesses are still checked in each iteration.
__attribute__((noinline)) static void fill(long *t) {
  t[0] = 1;
  t[1] = 2;
}
long lifetime_loop(long *p, long n) {
  long s = 0;
  for (long i = 0; i < n; i++) {
    long t[2];
    fill(t);
    s += p[i] + t[1];
  }
  return s;
}
// CHECK-LABEL: define {{.*}} @lifetime_loop(
// CHECK-NOT: call void @__asan_loadN(
// CHECK: call void @__asan_report_load8(
// CHECK-NOT: call void @__asan_loadN(
// CHECK: {{^}$}}

// A variable-length array in the loop: ASan unpoisons it at the stackrestore
// that ends each iteration.
long vla_loop(long *p, long n, long m) {
  long s = 0;
  for (long i = 0; i < n; i++) {
    long v[m];
    fill(v);
    s += p[i] + v[1];
  }
  return s;
}
// CHECK-LABEL: define {{.*}} @vla_loop(
// CHECK-NOT: call void @__asan_loadN(
// CHECK: call void @__asan_report_load8(
// CHECK: call void @__asan_allocas_unpoison(
// CHECK-NOT: call void @__asan_loadN(
// CHECK: {{^}$}}

void ext(void);
long ext_loop(long *p, long n) {
  long s = 0;
  for (long i = 0; i < n; i++) {
    s += p[i];
    ext();
  }
  return s;
}
// CHECK-LABEL: define {{.*}} @ext_loop(
// CHECK-NOT: call void @__asan_loadN(
// CHECK: call void @__asan_report_load8(
// CHECK-NOT: call void @__asan_loadN(
// CHECK: {{^}$}}

// With -fsanitize=fuzzer-no-link, SanitizerCoverage puts a comparison
// callback in each iteration.
long sum_cmp(const long *a, long n, long k) {
  long s = 0;
  for (long i = 0; i < n; i++)
    if (a[i] != k)
      s += a[i];
  return s;
}
// FUZZ-LABEL: define {{.*}} @sum_cmp(
// FUZZ-NOT: call void @__asan_report_load8
// FUZZ: call void @__sanitizer_cov_trace_cmp8(
// FUZZ-NOT: call void @__asan_report_load8
// FUZZ: call void @__asan_loadN(
// FUZZ-NOT: call void @__asan_report_load8
// FUZZ: {{^}$}}
