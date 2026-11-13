// LocationMerge folds accesses to adjacent bytes of one base into a single
// observation. ASan now checks that merged range once instead of falling back
// to one native check per access. The merged range never has a gap (MAIR only
// merges overlapping or adjacent ranges, AbstractAccess::merge), so checking
// every byte of it reports nothing the original accesses would not.
//
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O1 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STATS

// Two 4-byte stores become one 8-byte range. Its start is only 4-byte
// aligned, so one shadow load cannot cover it; like native ASan for an
// unaligned 8-byte access, the inline check tests its first and last byte.
// CHECK-LABEL: define {{.*}} @store_pair(
// CHECK-NOT:   __asan_report_store4
// CHECK:       call void @__asan_report_store_n(i64 %{{.*}}, i64 8)
// CHECK:       call void @__asan_report_store_n(i64 %{{.*}}, i64 8)
// CHECK-NOT:   __asan_report_store4
// CHECK:       ret void
void store_pair(int *p) {
  p[0] = 1;
  p[1] = 2;
}

// Two 8-byte stores at an 8-byte aligned start become one 16-byte range, which
// the inline fast path checks with a single 2-byte shadow load.
// CHECK-LABEL: define {{.*}} @store_long_pair(
// CHECK-NOT:   __asan_report_store8
// CHECK:       load i16, ptr
// CHECK:       call void @__asan_report_store16(
// CHECK-NOT:   __asan_report_store8
// CHECK:       ret void
void store_long_pair(long *p) {
  p[0] = 1;
  p[1] = 2;
}

// ASan checks reads and writes alike, so a read and a write merge. The range
// is reported as a store because one of its accesses writes.
// CHECK-LABEL: define {{.*}} @copy_next(
// CHECK-NOT:   __asan_report_load
// CHECK:       call void @__asan_report_store_n(i64 %{{.*}}, i64 8)
// CHECK:       call void @__asan_report_store_n(i64 %{{.*}}, i64 8)
// CHECK-NOT:   __asan_report_load
// CHECK:       ret void
void copy_next(int *p) { p[1] = p[0]; }

// A read-modify-write of the same bytes is one 4-byte check. It is reported
// as a READ, like the load native ASan checks first.
// CHECK-LABEL: define {{.*}} @increment(
// CHECK:       call void @__asan_report_load4(
// CHECK-NOT:   __asan_report_
// CHECK:       ret void
void increment(int *p) { ++*p; }

// Accesses of different sizes, 12 bytes in all.
// CHECK-LABEL: define {{.*}} @store_mixed(
// CHECK-NOT:   __asan_report_store{{[0-9]}}
// CHECK:       call void @__asan_report_store_n(i64 %{{.*}}, i64 12)
// CHECK:       call void @__asan_report_store_n(i64 %{{.*}}, i64 12)
// CHECK-NOT:   __asan_report_store{{[0-9]}}
// CHECK:       ret void
void store_mixed(char *p) {
  *(int *)p = 1;
  *(long *)(p + 4) = 2;
}

// Beyond 16 bytes the first and last byte could both be addressable around a
// redzone in between, so __asan_storeN checks every byte of the range.
// CHECK-LABEL: define {{.*}} @store_row(
// CHECK-NOT:   __asan_report_store
// CHECK:       call void @__asan_storeN(i64 %{{.*}}, i64 20)
// CHECK-NOT:   __asan_report_store
// CHECK:       ret void
void store_row(int *p) {
  p[0] = 1;
  p[1] = 2;
  p[2] = 3;
  p[3] = 4;
  p[4] = 5;
}

// A symbolic offset with a constant size is still one ordinary access; MAIR
// computes its address and ASan checks it inline as before.
// CHECK-LABEL: define {{.*}} @load_indexed(
// CHECK:       call void @__asan_report_load4(
// CHECK-NOT:   __asan_report_
// CHECK:       ret i32
int load_indexed(int *p, long i) { return p[i]; }

// Every observation above is lowered through MAIR; none falls back.
// STATS: MAIR stats
// STATS: select{{ +}}in 7  lowered 7{{$}}
