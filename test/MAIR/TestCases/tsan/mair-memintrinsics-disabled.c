// With -ts-instrument-memintrinsics=0, TSan records no memcpy, so the memcpy
// cannot stand in for the reads after it: they are recorded as natively.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=DEFAULT
// RUN: env MAIR=1 %clang -tsan -O1 -mllvm -ts-instrument-memintrinsics=0 \
// RUN:   -S -emit-llvm %s -o - | FileCheck %s --check-prefix=OFF

long f(char *d, const char *s) {
  __builtin_memcpy(d, s, 24);
  return d[3] + s[5];
}
// By default the recorded memcpy covers both reads.
// DEFAULT-LABEL: define {{.*}} @f(
// DEFAULT: call ptr @__tsan_memcpy(ptr %d, ptr %s, i64 24)
// DEFAULT-NOT: call void @__tsan_read
// DEFAULT: ret i64

// OFF-LABEL: define {{.*}} @f(
// OFF-NOT: call ptr @__tsan_memcpy
// OFF: call void @llvm.memcpy
// OFF: call void @__tsan_read1(ptr %{{.*}})
// OFF: call void @__tsan_read1(ptr %{{.*}})
// OFF: ret i64
