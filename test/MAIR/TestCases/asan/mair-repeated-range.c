// Two loops over the same n elements each leave one range observation after
// periodic combination. Their sizes are the same expression of n, built
// separately for each loop. The first range dominates the second, so ROE drops
// the second: one check of the bytes after the first loop.
// RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s

long two_loops(long *p, long n) {
  if (n <= 0)
    return 0;
  for (long i = 0; i < n; i++)
    p[i] = i;
  long s = 0;
  for (long i = 0; i < n; i++)
    s += p[i];
  return s;
}
// CHECK-LABEL: define {{.*}} @two_loops(
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       call void @__asan_storeN(
// CHECK-NOT:   call void @__asan_{{load|store|report}}
// CHECK:       ret i64
