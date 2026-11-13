// RUN: env MAIR=1 %clang -asan -O1 -g -S -emit-llvm %s -o - | FileCheck %s

// ASan does not tell reads from writes, so MAIR may keep one check for a load
// and a store, or merge them into one range. The check reports the access
// that runs first, as native ASan would on its first check: its kind and its
// source line.

int x;

// The load and the store of *p cover each other, and nothing between them
// stops that. The load runs first and keeps the check.
// CHECK-LABEL: define {{.*}} @read_then_write(
// CHECK:       call void @__asan_report_load4({{.*}}){{.*}}, !dbg [[READ_THEN_WRITE:![0-9]+]]
// CHECK-NOT:   call void @__asan_report_
// CHECK:       ret void
void read_then_write(int *p, int c) {
  int a = *p; // READ_THEN_WRITE
  if (c)
    x = 1;
  *p = a + 1;
}

// The two accesses merge into one 8-byte range. LocationMerge orders the
// sources by offset, so the store comes first among them, but the load runs
// first: the range is reported as a load, at the load.
// CHECK-LABEL: define {{.*}} @copy_prev(
// CHECK-NOT:   call void @__asan_report_store
// CHECK:       call void @__asan_report_load_n({{.*}}, i64 8){{.*}}, !dbg [[COPY_PREV:![0-9]+]]
// CHECK:       call void @__asan_report_load_n({{.*}}, i64 8){{.*}}, !dbg [[COPY_PREV]]
// CHECK-NOT:   call void @__asan_report_store
// CHECK:       ret void
void copy_prev(int *p) {
  int v = p[1]; // COPY_PREV
  p[0] = v;
}

// x[i]++ in a loop: the load and the store of each element cover each other,
// and the load keeps the check. The loop's elements become one periodic
// range, checked after the loop as a load.
// CHECK-LABEL: define {{.*}} @bump_strided(
// CHECK-NOT:   call void @__asan_store_periodic
// CHECK:       call void @__asan_load_periodic(i64 %{{.*}}, i64 8, i64 4, i64 %{{.*}})
// CHECK-NOT:   call void @__asan_store_periodic
// CHECK:       ret void
void bump_strided(int *p, long n) {
  for (long i = 0; i < n; ++i)
    p[2 * i] += 1;
}

// CHECK-DAG: [[READ_THEN_WRITE]] = !DILocation(line: 17,
// CHECK-DAG: [[COPY_PREV]] = !DILocation(line: 33,
