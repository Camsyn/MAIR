// LoopPeriodicObservationCombine replaces the per-iteration observation of a
// loop access by one observation at the loop exit. When the iterations cover
// contiguous bytes, TSan records them as one range access there instead of one
// access per iteration.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s

// A constant trip count gives a constant range.
void fill(int *p) {
  for (int i = 0; i < 100; ++i)
    p[i] = i;
}
// CHECK-LABEL: define {{.*}} @fill(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: call void @__tsan_write_range(ptr %p, i64 400)
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret void

// A symbolic trip count gives a range whose size is computed at the exit.
int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i];
  return s;
}
// CHECK-LABEL: define {{.*}} @sum(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: call void @__tsan_read_range(ptr %p, i64 %{{.*}})
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret i32

// A downward loop is a periodic region with a negative stride as wide as the
// access. It still covers contiguous bytes, which start at the last trip.
void fill_down(int *p, int n) {
  for (int i = n - 1; i >= 0; --i)
    p[i] = i;
}
// CHECK-LABEL: define {{.*}} @fill_down(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: call void @__tsan_write_range(ptr %{{.*}}, i64 %{{.*}})
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret void

// A stride wider than the access leaves gaps. One range would record bytes no
// iteration touches, so each iteration keeps its own access.
struct S {
  int x, y;
  long z;
};
int sum_x(struct S *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i].x;
  return s;
}
// CHECK-LABEL: define {{.*}} @sum_x(
// CHECK-NOT: call void @__tsan_{{read|write}}_range
// CHECK: for.body:
// CHECK: call void @__tsan_read4(
// CHECK-NOT: call void @__tsan_{{read|write}}_range
// CHECK: {{^}$}}
