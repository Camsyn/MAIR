; RUN: mair-opt -Og %s | FileCheck %s
; RUN: mair-opt -Og --disable-output %s
; RUN: mair-opt -Og %s > %t.og
; RUN: mair-opt -passes="default<Og>" %s > %t.default
; RUN: diff -u %t.og %t.default
; RUN: not mair-opt -Og -passes=simplifygraph %s 2>&1 | FileCheck %s --check-prefix=CONFLICT

; CONFLICT: -Og/-O1 cannot be combined with -passes

define void @og_keeps_branch_accesses(ptr %p, ptr %q, i1 %cond) {
; CHECK-LABEL: mair graph of fn @og_keeps_branch_accesses {
; CHECK:       read ptr %p, 4
; CHECK:       read ptr %q, 4
; CHECK:       read ptr %p, 4
entry:
  br i1 %cond, label %left, label %right

left:
  %left.common = load i32, ptr %p, align 4
  %left.private = load i32, ptr %q, align 4
  ret void

right:
  %right.common = load i32, ptr %p, align 4
  ret void
}

define void @og_preserves_precise_locations(ptr %p) {
; CHECK-LABEL: mair graph of fn @og_preserves_precise_locations {
; CHECK:       read ptr %p, 4
; CHECK:       write ptr %p + 4, 4
entry:
  %v = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  store i32 %v, ptr %p4, align 4
  ret void
}

define void @og_includes_provenance_preserving_loop_transforms(ptr %p,
                                                                i32 %n) {
; CHECK-LABEL: mair graph of fn @og_includes_provenance_preserving_loop_transforms {
; CHECK:       be1: preds=
; CHECK-NEXT:    read ptr %p, 4
; CHECK-NEXT:    read ptr %p, 4
; CHECK:       ma3: preds=
; CHECK-EMPTY:
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %a = load i32, ptr %p, align 4
  %b = load i32, ptr %p, align 4
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  %next = add i32 %i, 1
  br label %header

exit:
  ret void
}

define void @og_includes_periodic_combine(ptr %p) {
; CHECK-LABEL: mair graph of fn @og_includes_periodic_combine {
; CHECK:       bs6: preds=
; CHECK-NEXT:    read ptr %p, 4
; CHECK-NEXT:    read ptr %p, 4
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %a = load i8, ptr %pi, align 1
  %b = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header

exit:
  ret void
}
