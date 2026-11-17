// A merged range of a constant size above 16 bytes is checked inline before
// any runtime call: the fast path reads the shadow of every granule the range
// touches, in at most 8-byte loads, and checks the partial granule at the end
// by ASan's rule. Only if it fails does __asan_loadN/storeN run, which finds
// the poisoned byte and reports it as before. Callbacks mode, and ranges above
// -as-mair-max-inline-range-size, call the runtime directly.
//
// RUN: env MAIR=1 %clang -asan -O1 -fno-slp-vectorize -S -emit-llvm %s -o - | FileCheck %s
// RUN: env MAIR=1 %clang -asan -O1 -fno-slp-vectorize -S -emit-llvm %s -o - \
// RUN:   -mllvm -as-instrumentation-with-call-threshold=0 \
// RUN:   | FileCheck %s --check-prefix=CALLS
// RUN: env MAIR=1 %clang -asan -O1 -fno-slp-vectorize -S -emit-llvm %s -o - \
// RUN:   -mllvm -as-mair-max-inline-range-size=0 \
// RUN:   | FileCheck %s --check-prefix=CALLS

struct s24 {
  long a, b, c;
};
struct s32 {
  long a, b, c, d;
};
struct s40 {
  long a, b, c, d, e;
};
struct s64 {
  long f[8];
};
struct s20 {
  long a, b;
  int c;
};
struct s136 {
  long f[17];
};

// 24 bytes from an 8-byte aligned start: 3 shadow bytes, read as two
// overlapping 2-byte loads.
// CHECK-LABEL: define {{.*}} @load24(
// CHECK:       [[S:%.*]] = add i64 {{%.*}}, 2147450880
// CHECK:       [[P0:%.*]] = inttoptr i64 [[S]] to ptr
// CHECK:       [[V0:%.*]] = load i16, ptr [[P0]], align 1
// CHECK:       [[S1:%.*]] = add i64 [[S]], 1
// CHECK:       [[P1:%.*]] = inttoptr i64 [[S1]] to ptr
// CHECK:       [[V1:%.*]] = load i16, ptr [[P1]], align 1
// CHECK:       [[OR:%.*]] = or i16 [[V0]], [[V1]]
// CHECK:       [[BAD:%.*]] = icmp ne i16 [[OR]], 0
// CHECK:       br i1 [[BAD]], label %[[SLOW:.*]], label %[[CONT:.*]], !prof
// CHECK:       [[SLOW]]:
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 24)
// CHECK-NEXT:  br label %[[CONT]]
// CHECK-NOT:   __asan_
// CHECK:       ret i64
// CALLS-LABEL: define {{.*}} @load24(
// CALLS-NOT:   load i16
// CALLS:       call void @__asan_loadN(i64 %{{.*}}, i64 24)
// CALLS-NOT:   __asan_
// CALLS:       ret i64
long load24(struct s24 *s) { return s->a + s->b + s->c; }

// 32 bytes: one 4-byte shadow load.
// CHECK-LABEL: define {{.*}} @store32(
// CHECK:       [[V:%.*]] = load i32, ptr {{%.*}}, align 1
// CHECK-NEXT:  [[BAD:%.*]] = icmp ne i32 [[V]], 0
// CHECK-NEXT:  br i1 [[BAD]]
// CHECK:       call void @__asan_storeN(i64 %{{.*}}, i64 32)
// CHECK-NOT:   __asan_
// CHECK:       ret void
// CALLS-LABEL: define {{.*}} @store32(
// CALLS-NOT:   load i32
// CALLS:       call void @__asan_storeN(i64 %{{.*}}, i64 32)
void store32(struct s32 *s) {
  s->a = 1;
  s->b = 2;
  s->c = 3;
  s->d = 4;
}

// 40 bytes: 5 shadow bytes, two overlapping 4-byte loads.
// CHECK-LABEL: define {{.*}} @load40(
// CHECK:       [[V0:%.*]] = load i32, ptr {{%.*}}, align 1
// CHECK:       add i64 {{%.*}}, 1
// CHECK:       [[V1:%.*]] = load i32, ptr {{%.*}}, align 1
// CHECK:       [[OR:%.*]] = or i32 [[V0]], [[V1]]
// CHECK:       icmp ne i32 [[OR]], 0
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 40)
// CHECK-NOT:   __asan_
// CHECK:       ret i64
long load40(struct s40 *s) { return s->a + s->b + s->c + s->d + s->e; }

// 64 bytes: one 8-byte shadow load.
// CHECK-LABEL: define {{.*}} @store64(
// CHECK:       [[V:%.*]] = load i64, ptr {{%.*}}, align 1
// CHECK-NEXT:  [[BAD:%.*]] = icmp ne i64 [[V]], 0
// CHECK-NEXT:  br i1 [[BAD]]
// CHECK:       call void @__asan_storeN(i64 %{{.*}}, i64 64)
// CHECK-NOT:   __asan_
// CHECK:       ret void
void store64(struct s64 *s) {
  for (int i = 0; i < 8; ++i)
    s->f[i] = i;
}

// 20 bytes from an aligned start: two full granules and a 4-byte tail. The
// tail's shadow is fine if 0 or above 3, the offset of the last byte.
// CHECK-LABEL: define {{.*}} @load20(
// CHECK:       [[S:%.*]] = add i64 {{%.*}}, 2147450880
// CHECK:       [[SL:%.*]] = add i64 [[S]], 2
// CHECK:       [[V:%.*]] = load i16, ptr {{%.*}}, align 1
// CHECK:       [[FULL:%.*]] = icmp ne i16 [[V]], 0
// CHECK:       [[PL:%.*]] = inttoptr i64 [[SL]] to ptr
// CHECK:       [[T:%.*]] = load i8, ptr [[PL]], align 1
// CHECK:       [[NZ:%.*]] = icmp ne i8 [[T]], 0
// CHECK:       [[GE:%.*]] = icmp sge i8 3, [[T]]
// CHECK:       [[TB:%.*]] = and i1 [[NZ]], [[GE]]
// CHECK:       [[BAD:%.*]] = or i1 [[FULL]], [[TB]]
// CHECK:       br i1 [[BAD]]
// CHECK:       call void @__asan_loadN(i64 %{{.*}}, i64 20)
// CHECK-NOT:   __asan_
// CHECK:       ret i64
long load20(struct s20 *s) { return s->a + s->b + s->c; }

// 20 bytes from a 4-byte aligned start: the shadow offsets come from the
// address. The granules before the one holding the last byte are 2 or 3: a
// 2-byte load from the first and one ending before the last cover them. The
// last granule is checked against the offset of the last byte in it.
// CHECK-LABEL: define {{.*}} @store_row(
// CHECK:       [[A:%.*]] = ptrtoint ptr %p to i64
// CHECK:       [[SH:%.*]] = lshr i64 [[A]], 3
// CHECK:       [[S:%.*]] = add i64 [[SH]], 2147450880
// CHECK:       [[LAST:%.*]] = add i64 [[A]], 19
// CHECK:       [[LSH:%.*]] = lshr i64 [[LAST]], 3
// CHECK:       [[SL:%.*]] = add i64 [[LSH]], 2147450880
// CHECK:       [[OFF:%.*]] = and i64 [[LAST]], 7
// CHECK:       [[OFF8:%.*]] = trunc i64 [[OFF]] to i8
// CHECK:       [[P0:%.*]] = inttoptr i64 [[S]] to ptr
// CHECK:       [[V0:%.*]] = load i16, ptr [[P0]], align 1
// CHECK:       [[S1:%.*]] = add i64 [[SL]], -2
// CHECK:       [[P1:%.*]] = inttoptr i64 [[S1]] to ptr
// CHECK:       [[V1:%.*]] = load i16, ptr [[P1]], align 1
// CHECK:       [[OR:%.*]] = or i16 [[V0]], [[V1]]
// CHECK:       [[FULL:%.*]] = icmp ne i16 [[OR]], 0
// CHECK:       [[PL:%.*]] = inttoptr i64 [[SL]] to ptr
// CHECK:       [[T:%.*]] = load i8, ptr [[PL]], align 1
// CHECK:       [[NZ:%.*]] = icmp ne i8 [[T]], 0
// CHECK:       [[GE:%.*]] = icmp sge i8 [[OFF8]], [[T]]
// CHECK:       [[TB:%.*]] = and i1 [[NZ]], [[GE]]
// CHECK:       [[BAD:%.*]] = or i1 [[FULL]], [[TB]]
// CHECK:       br i1 [[BAD]]
// CHECK:       call void @__asan_storeN(i64 %{{.*}}, i64 20)
// CHECK-NOT:   __asan_
// CHECK:       ret void
void store_row(int *p) {
  p[0] = 1;
  p[1] = 2;
  p[2] = 3;
  p[3] = 4;
  p[4] = 5;
}

// Above the default limit of 128 bytes the runtime is called directly.
// CHECK-LABEL: define {{.*}} @store136(
// CHECK-NOT:   load i
// CHECK:       call void @__asan_storeN(i64 %{{.*}}, i64 136)
// CHECK-NOT:   __asan_
// CHECK:       ret void
void store136(struct s136 *s) {
  for (int i = 0; i < 17; ++i)
    s->f[i] = i;
}
