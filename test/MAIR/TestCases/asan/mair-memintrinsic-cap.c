// Native ASan stops instrumenting a basic block after -asan-max-ins-per-bb
// accesses and memory intrinsics. A memcpy it leaves alone checks nothing, so
// it cannot stand in for an access in a later block, which native ASan checks.
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=DEFAULT
// RUN: env MAIR=1 %clang -asan -O1 -mllvm -asan-max-ins-per-bb=1 -S -emit-llvm \
// RUN:   %s -o - | FileCheck %s --check-prefix=CAP

long g(char *d, const char *s, long n) {
  long x = d[3];
  __builtin_memcpy(d, s, 24);
  if (n)
    x += d[5];
  return x;
}
// By default the checked memcpy covers d[5].
// DEFAULT-LABEL: define {{.*}} @g(
// DEFAULT: call void @__asan_report_load1(
// DEFAULT: call ptr @__asan_memcpy(
// DEFAULT-NOT: call void @__asan_report_load1(
// DEFAULT: ret i64

// With a cap of one, only d[3] is checked in the first block.
// CAP-LABEL: define {{.*}} @g(
// CAP: call void @__asan_report_load1(
// CAP-NOT: call ptr @__asan_memcpy(
// CAP: call void @llvm.memcpy
// CAP: call void @__asan_report_load1(
// CAP: ret i64
