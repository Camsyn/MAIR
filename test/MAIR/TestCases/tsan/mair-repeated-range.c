// Two loops over the same n elements each leave one range observation after
// periodic combination. Their sizes are the same expression of n, built
// separately for each loop. The write range dominates the read range and
// covers it, so TSan records only the write range.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s

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
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       call void @__tsan_write_range(ptr %p,
// CHECK-NOT:   call void @__tsan_{{read|write|unaligned}}
// CHECK:       ret i64
