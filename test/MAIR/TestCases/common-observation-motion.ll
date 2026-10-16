; RUN: mair-opt -passes=simplifygraph,com %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph,com --disable-output %s
; RUN: mair-opt -passes=simplifygraph %s | FileCheck %s --check-prefix=SIMPLIFY

; Raw construction keeps observations on memory-instruction program points. COM is
; boundary-only, so these cases verify that it does not search descendants.

define void @hoist_diamond(ptr %p, ptr %q, i1 %cond) {
; CHECK-LABEL: mair graph of fn @hoist_diamond {
; CHECK:       pp4: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK-NEXT:    o: read ptr %q, 4
; CHECK:       pp8: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK-NEXT:    o: read ptr %p + 4, 4
; SIMPLIFY-LABEL: mair graph of fn @hoist_diamond {
; SIMPLIFY:       pp4: preds=
; SIMPLIFY-NEXT:    o: read ptr %p, 4
; SIMPLIFY:       pp8: preds=
; SIMPLIFY-NEXT:    o: read ptr %p, 4
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

define void @sink_diamond(ptr %p, ptr %q, i1 %cond) {
; CHECK-LABEL: mair graph of fn @sink_diamond {
; CHECK:       pp8: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK-NEXT:    o: read ptr %q, 4
; CHECK:       pp12: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK-NEXT:    o: read ptr %p + 4, 4
; CHECK:       pp14: preds=
; CHECK-EMPTY:
entry:
  br i1 %cond, label %left.path, label %right.path

left.path:
  br label %left

right.path:
  br label %right

left:
  %left.common = load i32, ptr %p, align 4
  %left.private = load i32, ptr %q, align 4
  br label %merge

right:
  %right.common = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  %right.private = load i32, ptr %p4, align 4
  br label %merge

merge:
  ret void
}

define void @hoist_nested_fixed_point(ptr %p, i1 %outer, i1 %inner) {
; CHECK-LABEL: mair graph of fn @hoist_nested_fixed_point {
; CHECK:       pp1: preds=
; CHECK-EMPTY:
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp8: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp11: preds=
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br i1 %outer, label %left, label %right

left:
  br i1 %inner, label %left.a, label %left.b

left.a:
  %a = load i32, ptr %p, align 4
  ret void

left.b:
  %b = load i32, ptr %p, align 4
  ret void

right:
  %r = load i32, ptr %p, align 4
  ret void
}

define i32 @hoist_loop_successor_quotient(ptr %p, i32 %n) {
; CHECK-LABEL: mair graph of fn @hoist_loop_successor_quotient {
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp8: preds=
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  %body.value = load i32, ptr %p, align 4
  %next = add i32 %i, 1
  br label %header

exit:
  %exit.value = load i32, ptr %p, align 4
  ret i32 %exit.value
}

define void @keep_non_common(ptr %p, ptr %q, i1 %cond) {
; CHECK-LABEL: mair graph of fn @keep_non_common {
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: write ptr %p, 4
; CHECK-NEXT:    o: read ptr %q, 4
entry:
  br i1 %cond, label %left, label %right

left:
  %left.value = load i32, ptr %p, align 4
  ret void

right:
  store i32 0, ptr %p, align 4
  %right.value = load i32, ptr %q, align 4
  ret void
}

define void @keep_single_successor(ptr %p) {
; CHECK-LABEL: mair graph of fn @keep_single_successor {
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br label %next

next:
  %value = load i32, ptr %p, align 4
  ret void
}

define void @hoist_target_local_symbol(ptr %p, i64 %i, i1 %cond) {
; CHECK-LABEL: mair graph of fn @hoist_target_local_symbol {
; CHECK:       pp1: preds=
; CHECK-EMPTY:
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %p + %pi, 4
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: read ptr %p + %pi, 4
entry:
  %pi = getelementptr i8, ptr %p, i64 %i
  br i1 %cond, label %left, label %right

left:
  %left.value = load i32, ptr %pi, align 4
  ret void

right:
  %right.value = load i32, ptr %pi, align 4
  ret void
}

define void @hoist_switch_unique_successors(ptr %p, i32 %tag) {
; CHECK-LABEL: mair graph of fn @hoist_switch_unique_successors {
; CHECK:       pp1: preds=
; CHECK-EMPTY:
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  switch i32 %tag, label %right [
    i32 0, label %left
    i32 1, label %left
  ]

left:
  %left.value = load i32, ptr %p, align 4
  ret void

right:
  %right.value = load i32, ptr %p, align 4
  ret void
}

define void @sink_nested_fixed_point(ptr %p, i1 %outer, i1 %inner) {
; CHECK-LABEL: mair graph of fn @sink_nested_fixed_point {
; CHECK:       pp13: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp16: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp21: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp23: preds=
; CHECK-EMPTY:
entry:
  br i1 %outer, label %left.path, label %right.path

left.path:
  br label %left

right.path:
  br label %right

left:
  br i1 %inner, label %left.a.path, label %left.b.path

left.a.path:
  br label %left.a

left.b.path:
  br label %left.b

left.a:
  %a = load i32, ptr %p, align 4
  br label %left.merge

left.b:
  %b = load i32, ptr %p, align 4
  br label %left.merge

left.merge:
  br label %final

right:
  %r = load i32, ptr %p, align 4
  br label %final

final:
  ret void
}

define i32 @keep_self_loop(ptr %p, i1 %done) {
; CHECK-LABEL: mair graph of fn @keep_self_loop {
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: read ptr %p, 4
entry:
  br label %header

header:
  %loop.value = load i32, ptr %p, align 4
  br i1 %done, label %exit, label %header

exit:
  %exit.value = load i32, ptr %p, align 4
  ret i32 %exit.value
}

define void @keep_different_offset(ptr %p, i1 %cond) {
; CHECK-LABEL: mair graph of fn @keep_different_offset {
; CHECK:       pp1: preds=
; CHECK-EMPTY:
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: read ptr %p + 4, 4
entry:
  br i1 %cond, label %left, label %right

left:
  %left.value = load i32, ptr %p, align 4
  ret void

right:
  %p4 = getelementptr i8, ptr %p, i64 4
  %right.value = load i32, ptr %p4, align 4
  ret void
}

define void @keep_different_size(ptr %p, i1 %cond) {
; CHECK-LABEL: mair graph of fn @keep_different_size {
; CHECK:       pp1: preds=
; CHECK-EMPTY:
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: read ptr %p, 8
entry:
  br i1 %cond, label %left, label %right

left:
  %left.value = load i32, ptr %p, align 4
  ret void

right:
  %right.value = load i64, ptr %p, align 8
  ret void
}
