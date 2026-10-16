; RUN: mair-opt -passes=liom %s | FileCheck %s
; RUN: mair-opt -passes=liom --disable-output %s
; RUN: mair-opt -passes=liom %s > %t.once
; RUN: mair-opt -passes=liom,liom %s > %t.twice
; RUN: diff -u %t.once %t.twice

define void @hoist_header_invariant(ptr %p, i32 %n) {
; CHECK-LABEL: mair graph of fn @hoist_header_invariant {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp3: preds=
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

define void @hoist_to_outer_preheader(ptr %p) {
; CHECK-LABEL: mair graph of fn @hoist_to_outer_preheader {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp5: preds=
; CHECK-EMPTY:
entry:
  br label %outer.header

outer.header:
  %oi = phi i32 [ 0, %entry ], [ %oi.next, %outer.latch ]
  br label %inner.header

inner.header:
  %ii = phi i32 [ 0, %outer.header ], [ %ii.next, %inner.header ]
  %v = load i32, ptr %p, align 4
  %ii.next = add i32 %ii, 1
  %inner.done = icmp eq i32 %ii.next, 2
  br i1 %inner.done, label %outer.latch, label %inner.header

outer.latch:
  %oi.next = add i32 %oi, 1
  %outer.done = icmp eq i32 %oi.next, 2
  br i1 %outer.done, label %exit, label %outer.header

exit:
  ret void
}

define void @sink_branch_only(ptr %p, i32 %n, i1 %cond) {
; CHECK-LABEL: mair graph of fn @sink_branch_only {
; CHECK:       pp1: preds=
; CHECK-EMPTY:
; CHECK:       pp7: preds=
; CHECK-EMPTY:
; CHECK:       pp13: site=bb.entry(%exit), guard=any(%v), preds=[], succs=[]
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  br i1 %cond, label %then, label %latch

then:
  %v = load i32, ptr %p, align 4
  br label %latch

latch:
  %next = add i32 %i, 1
  br label %header

exit:
  ret void
}

define void @sink_zero_trip_body(ptr %p, i32 %n) {
; CHECK-LABEL: mair graph of fn @sink_zero_trip_body {
; CHECK:       pp1: preds=
; CHECK-EMPTY:
; CHECK:       pp5: preds=
; CHECK-EMPTY:
; CHECK:       pp9: site=bb.entry(%exit), guard=any(%v), preds=[], succs=[]
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  %v = load i32, ptr %p, align 4
  %next = add i32 %i, 1
  br label %header

exit:
  ret void
}

define void @sink_branch_to_shared_exit(ptr %p, i32 %n, i1 %cond,
                                        i1 %bypass) {
; CHECK-LABEL: mair graph of fn @sink_branch_to_shared_exit {
; CHECK:       pp9: preds=
; CHECK-EMPTY:
; CHECK:       pp15: site=edge(%header -> %exit), guard=any(%v), preds=[], succs=[]
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br i1 %bypass, label %exit, label %preheader

preheader:
  br label %header

header:
  %i = phi i32 [ 0, %preheader ], [ %next, %latch ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  br i1 %cond, label %then, label %latch

then:
  %v = load i32, ptr %p, align 4
  br label %latch

latch:
  %next = add i32 %i, 1
  br label %header

exit:
  ret void
}

define void @hoist_with_multiple_exits(ptr %p, i32 %n, i1 %early) {
; CHECK-LABEL: mair graph of fn @hoist_with_multiple_exits {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp3: preds=
; CHECK-EMPTY:
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %v = load i32, ptr %p, align 4
  br i1 %early, label %exit.early, label %body

body:
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit.normal, label %header

exit.early:
  ret void

exit.normal:
  ret void
}

define void @sink_without_preheader(ptr %p, i1 %path, i32 %n) {
; CHECK-LABEL: mair graph of fn @sink_without_preheader {
; CHECK:       pp7: preds=
; CHECK-EMPTY:
; CHECK:       pp13: site=bb.entry(%exit), guard=any(%v), preds=[], succs=[]
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br i1 %path, label %left, label %right

left:
  br label %header

right:
  br label %header

header:
  %i = phi i32 [ 0, %left ], [ 0, %right ], [ %next, %latch ]
  %v = load i32, ptr %p, align 4
  %done = icmp eq i32 %i, %n
  br i1 %done, label %exit, label %latch

latch:
  %next = add i32 %i, 1
  br label %header

exit:
  ret void
}
