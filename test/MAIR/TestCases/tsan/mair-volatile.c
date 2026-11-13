// TSan treats volatile accesses as plain unless -ts-distinguish-volatile asks
// for __tsan_volatile_* calls, so MAIR optimizes volatile observations by
// default and keeps every one of them distinct otherwise.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=PLAIN
// RUN: env MAIR=1 %clang -tsan -O1 -mllvm -ts-distinguish-volatile -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=DISTINCT

// The write check covers the read before it.
void inc(volatile int *p) { *p = *p + 1; }

// PLAIN-LABEL: define {{.*}} @inc(
// PLAIN-NOT:   call void @__tsan_{{.*}}read
// PLAIN:       call void @__tsan_write4(ptr %p)
// PLAIN-NOT:   call void @__tsan_{{.*}}read
// PLAIN:       ret void

// DISTINCT-LABEL: define {{.*}} @inc(
// DISTINCT:       call void @__tsan_volatile_read4(ptr %p)
// DISTINCT:       call void @__tsan_volatile_write4(ptr %p)
// DISTINCT:       ret void

// A plain load and a volatile load of the same int need one check, unless
// volatile is distinguished.
int mixed(volatile int *p) { return *(int *)p + *p; }

// PLAIN-LABEL: define {{.*}} @mixed(
// PLAIN:       call void @__tsan_read4(ptr %p)
// PLAIN-NOT:   call void @__tsan_{{.*}}read
// PLAIN:       ret i32

// DISTINCT-LABEL: define {{.*}} @mixed(
// DISTINCT-DAG:   call void @__tsan_read4(ptr %p)
// DISTINCT-DAG:   call void @__tsan_volatile_read4(ptr %p)
// DISTINCT:       ret i32
