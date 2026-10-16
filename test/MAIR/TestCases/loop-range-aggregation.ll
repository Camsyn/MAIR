; RUN: mair-opt -passes=lpoc %s | FileCheck %s
; RUN: mair-opt -passes=lpoc --disable-output %s
; RUN: mair-opt -passes=lpoc %s > %t.once
; RUN: mair-opt -passes=lpoc,lpoc %s > %t.twice
; RUN: diff -u %t.once %t.twice

define void @aggregate_fixed_range(ptr %p) {
; CHECK-LABEL: mair graph of fn @aggregate_fixed_range {
; CHECK:       pp3: preds=
; CHECK-EMPTY:
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read ptr %p, 4
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
; CHECK:       pp3: preds=
; CHECK-EMPTY:
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read ptr %p, 4
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

define void @aggregate_non_contiguous_period(ptr %p) {
; CHECK-LABEL: mair graph of fn @aggregate_non_contiguous_period {
; CHECK:       pp3: preds=
; CHECK-EMPTY:
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read period start=%p, stride=2, access-size=1, trips=4
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

define void @aggregate_dynamic_header_count(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @aggregate_dynamic_header_count {
; CHECK:       pp3: preds=
; CHECK-EMPTY:
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: read period start=%p, stride=1, access-size=1, trips=(1 + zext(%n to i65))
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %body ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %more = icmp ult i64 %i, %n
  br i1 %more, label %body, label %exit

body:
  %next = add i64 %i, 1
  br label %header

exit:
  ret void
}

define void @aggregate_zero_trip_dynamic_body(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @aggregate_zero_trip_dynamic_body {
; CHECK:       pp5: preds=
; CHECK-EMPTY:
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: read period start=%p, stride=1, access-size=1, trips=zext(%n to i65)
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %body ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %body, label %exit

body:
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  br label %header

exit:
  ret void
}

define void @aggregate_dynamic_stride_body(ptr %p, i64 %n, i64 %stride) {
; CHECK-LABEL: mair graph of fn @aggregate_dynamic_stride_body {
; CHECK:       pp5: preds=
; CHECK-EMPTY:
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: read period start=%p, stride=%stride, access-size=1, trips=zext(%n to i65)
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %body ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %body, label %exit

body:
  %off = mul i64 %i, %stride
  %pi = getelementptr i8, ptr %p, i64 %off
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  br label %header

exit:
  ret void
}

define void @aggregate_dynamic_scev_ops(ptr %p, i64 %n, i32 %stride,
                                        i64 %denom) {
; CHECK-LABEL: mair graph of fn @aggregate_dynamic_scev_ops {
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: read period start=%p, stride=((3 * zext(%stride to i64)) udiv %denom), access-size=1, trips=zext(%n to i65)
entry:
  %wide = zext i32 %stride to i64
  %scaled = mul i64 %wide, 3
  %divided = udiv i64 %scaled, %denom
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %body ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %body, label %exit

body:
  %off = mul i64 %i, %divided
  %pi = getelementptr i8, ptr %p, i64 %off
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  br label %header

exit:
  ret void
}

define void @aggregate_shared_exit(ptr %p, i1 %bypass) {
; CHECK-LABEL: mair graph of fn @aggregate_shared_exit {
; CHECK:       pp5: preds=
; CHECK-EMPTY:
; CHECK:       pp11: site=edge(%header -> %exit), preds=[], succs=[]
; CHECK-NEXT:    o: read ptr %p, 4
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

define void @keep_branch_only_period(ptr %p, i64 %n, i1 %cond) {
; CHECK-LABEL: mair graph of fn @keep_branch_only_period {
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: read ptr %p + %pi, 1
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %body, label %exit

body:
  br i1 %cond, label %then, label %latch

then:
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  br label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret void
}

define void @aggregate_maximal_header_count(ptr %p) {
; CHECK-LABEL: mair graph of fn @aggregate_maximal_header_count {
; CHECK:       pp3: preds=
; CHECK-EMPTY:
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read period start=%p, stride=1, access-size=1, trips=18446744073709551616
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 0
  br i1 %done, label %exit, label %header

exit:
  ret void
}

define void @keep_nested_addrec_site(ptr %p) {
; CHECK-LABEL: mair graph of fn @keep_nested_addrec_site {
; CHECK:       o: read ptr %p + %pi, 1
entry:
  br label %outer.header

outer.header:
  %oi = phi i64 [ 0, %entry ], [ %oi.next, %inner.exit ]
  br label %inner.header

inner.header:
  %ii = phi i64 [ 0, %outer.header ], [ %ii.next, %inner.header ]
  %pi = getelementptr i8, ptr %p, i64 %oi
  %v = load i8, ptr %pi, align 1
  %ii.next = add i64 %ii, 1
  %inner.done = icmp eq i64 %ii.next, 2
  br i1 %inner.done, label %inner.exit, label %inner.header

inner.exit:
  %oi.next = add i64 %oi, 1
  %outer.done = icmp eq i64 %oi.next, 2
  br i1 %outer.done, label %exit, label %outer.header

exit:
  ret void
}

define void @keep_quadratic_pointer_recurrence(ptr %p) {
; CHECK-LABEL: mair graph of fn @keep_quadratic_pointer_recurrence {
; CHECK:       o: read ptr %p + %pi, 1
entry:
  br label %header

header:
  %iteration = phi i64 [ 0, %entry ], [ %next.iteration, %header ]
  %i = phi i64 [ 0, %entry ], [ %next.i, %header ]
  %step = phi i64 [ 1, %entry ], [ %next.step, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next.i = add i64 %i, %step
  %next.step = add i64 %step, 1
  %next.iteration = add i64 %iteration, 1
  %done = icmp eq i64 %next.iteration, 4
  br i1 %done, label %exit, label %header

exit:
  ret void
}

define void @keep_irreducible_period(ptr %p, i1 %entry.sel, i1 %cycle.sel,
                                     i1 %again) {
; CHECK-LABEL: mair graph of fn @keep_irreducible_period {
; CHECK:       o: read ptr %p + %pi, 1
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  br i1 %again, label %dispatch, label %exit

dispatch:
  br i1 %entry.sel, label %a, label %b

a:
  br i1 %cycle.sel, label %b, label %latch

b:
  br i1 %cycle.sel, label %a, label %latch

latch:
  %pi = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  br label %header

exit:
  ret void
}
