; RUN: mair-opt -passes=lpoc %s | FileCheck %s
; RUN: mair-opt -passes=lpoc --disable-output %s
; RUN: mair-opt -passes=lpoc %s > %t.once
; RUN: mair-opt -passes=lpoc,lpoc %s > %t.twice
; RUN: diff -u %t.once %t.twice

; Inner j in [0, 4) is one range [p + 4i, p + 4i + 4). The parent loop then
; folds that range, whose start is still the outer induction, into [p, p + 4n).
define void @nested_contiguous_rows(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @nested_contiguous_rows {
; CHECK:       pp14: loop-exit=%outer.header,
; CHECK-NEXT:    o: read ptr %p, (zext(%n to i65) * 4)
; CHECK-LABEL: mair graph of fn @nested_contiguous_rows of loop %outer.header {
; CHECK-NOT:   o:
; CHECK:       }
; CHECK-LABEL: mair graph of fn @nested_contiguous_rows of loop %inner.header {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  br label %outer.header

outer.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %outer.body, label %exit

outer.body:
  %row.off = shl i64 %i, 2
  %row = getelementptr i8, ptr %p, i64 %row.off
  br label %inner.header

inner.header:
  %j = phi i64 [ 0, %outer.body ], [ %j.next, %inner.header ]
  %ptr = getelementptr i8, ptr %row, i64 %j
  %v = load i8, ptr %ptr, align 1
  %j.next = add i64 %j, 1
  %inner.done = icmp eq i64 %j.next, 4
  br i1 %inner.done, label %outer.latch, label %inner.header

outer.latch:
  %i.next = add i64 %i, 1
  br label %outer.header

exit:
  ret void
}
