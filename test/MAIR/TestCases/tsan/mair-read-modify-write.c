// A read-modify-write of one location reads and writes the same bytes at one
// program point. Native TSan records only the write: the read happens before
// it on the same thread, so the write's shadow already conflicts with every
// race the read could report. MAIR keeps the write and drops the covered read,
// also when the address is a symbolic offset or a thread-local variable.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env -u MAIR %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s

void inc(int *p, long i) { p[i]++; }
// CHECK-LABEL: define {{.*}} @inc(
// CHECK-NOT: call void @__tsan_{{read|unaligned}}
// CHECK: call void @__tsan_write4(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret void

static _Thread_local long t;
void inc_tls(void) { t += 1; }
// CHECK-LABEL: define {{.*}} @inc_tls(
// CHECK-NOT: call void @__tsan_{{read|unaligned}}
// CHECK: call void @__tsan_write8(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret void
