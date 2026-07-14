; RUN: mair-opt -O1 %s | FileCheck %s
; RUN: mair-opt -O1 --disable-output %s
; RUN: mair-opt -O1 %s > %t.o1
; RUN: mair-opt -passes="default<O1>" %s > %t.default
; RUN: diff -u %t.o1 %t.default
; RUN: not mair-opt -O1 -passes=simplifygraph %s 2>&1 | FileCheck %s --check-prefix=CONFLICT

; CONFLICT: -Og/-O1 cannot be combined with -passes

define void @o1_common_before_location_merge(ptr %p, ptr %q, i1 %cond) {
; CHECK-LABEL: mair graph of fn @o1_common_before_location_merge {
; CHECK:       be1: preds=
; CHECK-NEXT:    read ptr %p, 4
; CHECK:       ma3: preds=
; CHECK-NEXT:    read ptr %q, 4
; CHECK:       ma7: preds=
; CHECK-NEXT:    read ptr %p + 4, 4
entry:
  br i1 %cond, label %left, label %right

left:
  %left.common = load i32, ptr %p, align 4
  %left.private = load i32, ptr %q, align 4
  ret void

right:
  %right.common = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  %right.private = load i32, ptr %p4, align 4
  ret void
}

define void @o1_location_merge_after_motion(ptr %p) {
; CHECK-LABEL: mair graph of fn @o1_location_merge_after_motion {
; CHECK:       ma1: preds=
; CHECK-NEXT:    read/write ptr %p, 12
entry:
  %p8 = getelementptr i8, ptr %p, i64 8
  %v8 = load i32, ptr %p8, align 4
  %v0 = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  store i32 %v0, ptr %p4, align 4
  ret void
}

define void @o1_loop_periodic_combine(ptr %p) {
; CHECK-LABEL: mair graph of fn @o1_loop_periodic_combine {
; CHECK:       ma3: preds=
; CHECK-EMPTY:
; CHECK:       bs5: preds=
; CHECK-NEXT:    read ptr %p, 4
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header

exit:
  ret void
}

define void @o1_loop_invariant_motion(ptr %p, i32 %n) {
; CHECK-LABEL: mair graph of fn @o1_loop_invariant_motion {
; CHECK:       be1: preds=
; CHECK-NEXT:    read ptr %p, 4
; CHECK:       ma3: preds=
; CHECK-EMPTY:
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %v = load i32, ptr %p, align 4
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  %next = add i32 %i, 1
  br label %header

exit:
  ret void
}

define void @o1_redundant_cross_bb(ptr %p) {
; CHECK-LABEL: mair graph of fn @o1_redundant_cross_bb {
; CHECK:       ma1: preds=
; CHECK-NEXT:    read ptr %p, 8
; CHECK:       ma6: preds=
; CHECK-NOT:     read ptr %p, 4
entry:
  %wide = load i64, ptr %p, align 8
  br label %empty

empty:
  br label %tail

tail:
  %narrow = load i32, ptr %p, align 4
  ret void
}
