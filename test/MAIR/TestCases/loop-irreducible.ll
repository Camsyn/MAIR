; RUN: mair-opt -passes='default<O1>' %s | FileCheck %s

; A non-standard cycle is not a reason to reject the function. Observations
; inside it stay at their original blocks. A separate natural loop is still
; optimized.

define void @keep_irreducible_period(ptr %p, i1 %entry.sel, i1 %cycle.sel,
                                     i1 %again) {
; CHECK-LABEL: mair graph of fn @keep_irreducible_period of loop %header {
; CHECK:       o: read ptr %p + %i, 1
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %n = add i64 %i, 4
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
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %header

exit:
  ret void
}

define i32 @side_entry(i32 %n, ptr %p) {
; CHECK-LABEL: mair graph of fn @side_entry {
; CHECK-NOT:   loop-preheader
; CHECK:       o: read ptr %p, 4
; CHECK:       o: write ptr %p, 4
; CHECK:       o: read ptr %p, 4
; CHECK:       o: write ptr %p, 4
entry:
  %zero = icmp eq i32 %n, 0
  br i1 %zero, label %mid, label %header

header:
  %n.loop = phi i32 [ %n, %entry ], [ %dec, %mid ]
  %cmp = icmp sgt i32 %n.loop, 0
  %dec = add i32 %n.loop, -1
  br i1 %cmp, label %body, label %exit

body:
  %a = load i32, ptr %p, align 4
  %a2 = add i32 %a, 1
  store i32 %a2, ptr %p, align 4
  br label %mid

mid:
  %b = load i32, ptr %p, align 4
  %b2 = add i32 %b, 2
  store i32 %b2, ptr %p, align 4
  br label %header

exit:
  ret i32 0
}
