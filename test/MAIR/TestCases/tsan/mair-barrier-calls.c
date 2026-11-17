// A call that can neither synchronize nor free memory (nosync and nofree) does
// not change what TSan records for the accesses around it, and neither do the
// SanitizerCoverage callbacks. Such calls are not barriers, so a loop that
// makes one is still recorded as one range access at its exit. Any other call
// still is a barrier: the loop is then recorded access by access.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -tsan -O1 -fsanitize=fuzzer-no-link -S -emit-llvm %s \
// RUN:   -o - | FileCheck %s --check-prefix=FUZZ

// llvm.smax: an intrinsic call with default attributes.
long smax_loop(long *p, long n) {
  long m = 0;
  for (long i = 0; i < n; i++)
    m = __builtin_elementwise_max(m, p[i]);
  return m;
}
// CHECK-LABEL: define {{.*}} @smax_loop(
// CHECK-NOT: call void @__tsan_read8
// CHECK: call void @__tsan_read_range(ptr %p, i64 %{{.*}})
// CHECK-NOT: call void @__tsan_read8
// CHECK: {{^}$}}

double fma_loop(double *p, double *q, long n) {
  double s = 0;
  for (long i = 0; i < n; i++)
    s = __builtin_fma(p[i], q[i], s);
  return s;
}
// CHECK-LABEL: define {{.*}} @fma_loop(
// CHECK-NOT: call void @__tsan_read8
// CHECK-DAG: call void @__tsan_read_range(ptr %p, i64 %{{.*}})
// CHECK-DAG: call void @__tsan_read_range(ptr %q, i64 %{{.*}})
// CHECK-NOT: call void @__tsan_read8
// CHECK: {{^}$}}

long assume_loop(long *p, long n) {
  long s = 0;
  for (long i = 0; i < n; i++) {
    long v = p[i];
    __builtin_assume(v != 5);
    s += v;
  }
  return s;
}
// The range is recorded in the exit block, which comes before the loop.
// CHECK-LABEL: define {{.*}} @assume_loop(
// CHECK-NOT: call void @__tsan_read8
// CHECK: call void @__tsan_read_range(ptr %p, i64 %{{.*}})
// CHECK-NOT: call void @__tsan_read8
// CHECK: call void @llvm.assume(
// CHECK-NOT: call void @__tsan_read8
// CHECK: {{^}$}}

// fill is inferred nosync, nofree and willreturn. The loop also starts and
// ends the lifetime of t in each iteration, which TSan does not track.
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
// CHECK-NOT: call void @__tsan_read8
// CHECK: call void @__tsan_read_range(ptr %p, i64 %{{.*}})
// CHECK-NOT: call void @__tsan_read8
// CHECK: call void @llvm.lifetime.start
// CHECK-NEXT: call {{.*}}void @fill(
// CHECK-NOT: call void @__tsan_read8
// CHECK: call void @llvm.lifetime.end
// CHECK-NOT: call void @__tsan_read8
// CHECK: {{^}$}}

// An external call may synchronize: each iteration is recorded on its own.
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
// CHECK-NOT: call void @__tsan_read_range
// CHECK: call void @__tsan_read8(
// CHECK: call void @ext()
// CHECK-NOT: call void @__tsan_read_range
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
// FUZZ-NOT: call void @__tsan_read8
// FUZZ: call void @__sanitizer_cov_trace_cmp8(
// FUZZ-NOT: call void @__tsan_read8
// FUZZ: call void @__tsan_read_range(ptr %a, i64 %{{.*}})
// FUZZ-NOT: call void @__tsan_read8
// FUZZ: {{^}$}}
