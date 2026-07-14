; RUN: mair-opt -passes=loop-range-aggregation %s | FileCheck %s
; RUN: mair-opt -passes=loop-range-aggregation --disable-output %s
; RUN: mair-opt -passes=loop-range-aggregation %s > %t.once
; RUN: mair-opt -passes=loop-range-aggregation,loop-range-aggregation %s > %t.twice
; RUN: diff -u %t.once %t.twice

define void @aggregate_fixed_range(ptr %p) {
; CHECK-LABEL: mair graph of fn @aggregate_fixed_range {
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

define void @aggregate_decreasing_range(ptr %p) {
; CHECK-LABEL: mair graph of fn @aggregate_decreasing_range {
; CHECK:       ma3: preds=
; CHECK-EMPTY:
; CHECK:       bs5: preds=
; CHECK-NEXT:    read ptr %p, 4
entry:
  br label %header

header:
  %i = phi i64 [ 3, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, -1
  %done = icmp eq i64 %next, -1
  br i1 %done, label %exit, label %header

exit:
  ret void
}

define void @keep_non_contiguous_period(ptr %p) {
; CHECK-LABEL: mair graph of fn @keep_non_contiguous_period {
; CHECK:       ma3: preds=
; CHECK-NEXT:    read ptr %p + %pi, 1
; CHECK:       bs5: preds=
; CHECK-EMPTY:
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %off = mul i64 %i, 2
  %pi = getelementptr i8, ptr %p, i64 %off
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header

exit:
  ret void
}

define void @keep_dynamic_range(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @keep_dynamic_range {
; CHECK:       ma3: preds=
; CHECK-NEXT:    read ptr %p + %pi, 1
; CHECK:       bs5: preds=
; CHECK-EMPTY:
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %header

exit:
  ret void
}

define void @keep_shared_exit(ptr %p, i1 %bypass) {
; CHECK-LABEL: mair graph of fn @keep_shared_exit {
; CHECK:       ma5: preds=
; CHECK-NEXT:    read ptr %p + %pi, 1
; CHECK:       bs9: preds=
; CHECK-EMPTY:
entry:
  br i1 %bypass, label %other, label %preheader

preheader:
  br label %header

header:
  %i = phi i64 [ 0, %preheader ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header

other:
  br label %exit

exit:
  ret void
}
