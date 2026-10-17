; RUN: not --crash mair-opt --disable-output %s 2>&1 | FileCheck %s
; RUN: not --crash mair-opt -passes=lpoc --disable-output %s 2>&1 | FileCheck %s

; The nested cycle has two entries and cannot be represented by LoopInfo.
; Refuse it at graph construction instead of treating it as a plain scope.
; CHECK: LLVM ERROR: MAIR does not support irreducible or unrepresented cycles

define void @keep_irreducible_period(ptr %p, i1 %entry.sel, i1 %cycle.sel,
                                     i1 %again) {
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
