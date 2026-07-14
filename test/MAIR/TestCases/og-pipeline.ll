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
