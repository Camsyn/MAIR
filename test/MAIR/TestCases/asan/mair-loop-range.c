// LoopPeriodicObservationCombine replaces the per-iteration observation of a
// loop access with one observation of everything the loop touches, placed
// outside the loop. ASan checks that footprint once, after the loop, instead of
// checking each iteration natively. Checking after the access is the existing
// MAIR rule for ASan (docs/mair-loops.md).
//
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O1 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STATS
// RUN: env MAIR=1 %clang -asan -O2 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=VEC

// p[0], ..., p[n-1] is a contiguous range with a constant start and a size
// computed from the trip count: one __asan_loadN after the loop.
// CHECK-LABEL: define {{.*}} @sum(
// CHECK-NOT:   __asan_report_load
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 %{{.*}})
// CHECK-NOT:   __asan_report_load
// CHECK:       ret i32
// At -O2 the loop is vectorized and interleaved: two 16-byte vector loads at
// +0 and +16 per 32-byte step. LocationMerge merges them in the loop body (and
// would merge their touching periodic regions after the loop otherwise), so
// one 32-byte access steps by its size: one range check from p covers the
// vector loop, even though its trip count is symbolic. The scalar remainder
// loop starts where no IR value points and gets a periodic check.
// VEC-LABEL: define {{.*}} @sum(
// VEC-NOT:   __asan_report_load
// VEC:       call void @__asan_loadN(i64 %{{.*}}, i64 %{{.*}})
// VEC:       call void @__asan_load_periodic(i64 %{{.*}}, i64 4, i64 4, i64 %{{.*}})
// VEC-NOT:   __asan_report_load
// VEC-NOT:   __asan_load_periodic
// VEC:       ret i32
int sum(int *p, int n) {
  int t = 0;
  for (int i = 0; i < n; ++i)
    t += p[i];
  return t;
}

// A symbolic start that no IR value computes makes the footprint a periodic
// region (start, stride 4, access size 4, trip count), checked by one runtime
// call.
// CHECK-LABEL: define {{.*}} @sum_from(
// CHECK-NOT:   __asan_report_load
// CHECK:       call void @__asan_load_periodic(i64 %{{.*}}, i64 4, i64 4, i64 %{{.*}})
// CHECK-NOT:   __asan_report_load
// CHECK:       ret i32
// At -O2 the vector loop starts at a GEP the vectorizer computes, so with a
// symbolic start and a symbolic trip count it is still one range check. The
// scalar remainder loop gets a periodic check.
// VEC-LABEL: define {{.*}} @sum_from(
// VEC-NOT:   __asan_report_load
// VEC:       call void @__asan_loadN(i64 %{{.*}}, i64 %{{.*}})
// VEC:       call void @__asan_load_periodic(i64 %{{.*}}, i64 4, i64 4, i64 %{{.*}})
// VEC-NOT:   __asan_report_load
// VEC-NOT:   __asan_load_periodic
// VEC:       ret i32
int sum_from(int *p, int b, int n) {
  int t = 0;
  for (int i = b; i < n; ++i)
    t += p[i];
  return t;
}

// A descending loop has a negative stride; start is the first iteration's
// address, the highest one. The runtime works out the low end.
// CHECK-LABEL: define {{.*}} @sum_down(
// CHECK-NOT:   __asan_report_load
// CHECK:       call void @__asan_load_periodic(i64 %{{.*}}, i64 -4, i64 4, i64 %{{.*}})
// CHECK-NOT:   __asan_report_load
// CHECK:       ret i32
int sum_down(int *p, int n) {
  int t = 0;
  for (int i = n - 1; i >= 0; --i)
    t += p[i];
  return t;
}

struct pair {
  int a, b;
};

// Reading one field of each element leaves 4-byte gaps (stride 8, access size
// 4). The runtime checks each element, never the gaps.
// CHECK-LABEL: define {{.*}} @sum_field(
// CHECK-NOT:   __asan_report_load
// CHECK:       call void @__asan_load_periodic(i64 %{{.*}}, i64 8, i64 4, i64 %{{.*}})
// CHECK-NOT:   __asan_report_load
// CHECK:       ret i32
int sum_field(struct pair *p, int n) {
  int t = 0;
  for (int i = 0; i < n; ++i)
    t += p[i].b;
  return t;
}

// Writes use the store variant.
// CHECK-LABEL: define {{.*}} @clear_field(
// CHECK-NOT:   __asan_report_store
// CHECK:       call void @__asan_store_periodic(i64 %{{.*}}, i64 8, i64 4, i64 %{{.*}})
// CHECK-NOT:   __asan_report_store
// CHECK:       ret void
void clear_field(struct pair *p, int b, int n) {
  for (int i = b; i < n; ++i)
    p[i].a = 0;
}

// STATS: MAIR stats
// STATS: select{{ +}}in 5  lowered 5{{$}}
