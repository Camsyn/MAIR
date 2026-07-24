; RUN: mair-opt -passes=roe %s | FileCheck %s
; RUN: mair-opt -passes=roe --disable-output %s
; RUN: mair-opt -passes="simplifygraph,roe" %s | FileCheck %s --check-prefix=INTRA

define void @remove_covered_accesses(ptr %p) {
; CHECK-LABEL: mair graph of fn @remove_covered_accesses {
; CHECK:       ma1: preds=
; CHECK-NEXT:    read ptr %p, 8
; CHECK:       ma2: preds=
; CHECK:       ma3: preds=
; CHECK:       ma4: preds=
; CHECK-NEXT:    write ptr %p, 4
entry:
  %wide = load i64, ptr %p, align 8
  %narrow = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  %tail = load i32, ptr %p4, align 4
  store i32 %narrow, ptr %p, align 4
  ret void
}

define void @remove_identical_symbolic_access(ptr %p, i64 %i) {
; CHECK-LABEL: mair graph of fn @remove_identical_symbolic_access {
; CHECK:       ma1: preds=
; CHECK-NOT:     read ptr %p + %pi, 4
; CHECK:       ma2: preds=
; CHECK-NEXT:    read ptr %p + %pi, 4
entry:
  %pi = getelementptr i8, ptr %p, i64 %i
  %a = load i32, ptr %pi, align 4
  %b = load i32, ptr %pi, align 4
  ret void
}

define void @keep_uncovered_accesses(ptr %p, ptr %q) {
; CHECK-LABEL: mair graph of fn @keep_uncovered_accesses {
; CHECK-NOT:     read ptr %p, 4
; CHECK:         read ptr %p + 8, 4
; CHECK-NOT:     read ptr %p, 4
; CHECK:         read ptr %q, 4
; CHECK-NOT:     read ptr %p, 4
; CHECK:         write ptr %p, 4
; INTRA-LABEL: mair graph of fn @keep_uncovered_accesses {
; INTRA:         ma4: preds=
; INTRA-NOT:       read ptr %p, 4
; INTRA-DAG:       read ptr %p + 8, 4
; INTRA-DAG:       read ptr %q, 4
; INTRA-DAG:       write ptr %p, 4
entry:
  %p0 = load i32, ptr %p, align 4
  %p8 = getelementptr i8, ptr %p, i64 8
  %far = load i32, ptr %p8, align 4
  %other = load i32, ptr %q, align 4
  store i32 %p0, ptr %p, align 4
  ret void
}

define void @cross_bb_diamond(ptr %p, ptr %q, i1 %cond) {
; CHECK-LABEL: mair graph of fn @cross_bb_diamond {
; CHECK:       ma1: preds=
; CHECK-NEXT:    read ptr %p, 8
; CHECK:       ma4: preds=
; CHECK-NOT:     read ptr %p, 4
; CHECK:       ma7: preds=
; CHECK-NOT:     read ptr %p + 4, 4
; CHECK:       ma10: preds=
; CHECK-NOT:     read ptr %p, 4
; CHECK:       ma11: preds=
; CHECK-NEXT:    read ptr %q, 4
; CHECK:       ma12: preds=
; CHECK-NEXT:    read ptr %p + 8, 4
; CHECK:       ma13: preds=
; CHECK-NEXT:    write ptr %p, 4
entry:
  %wide = load i64, ptr %p, align 8
  br i1 %cond, label %then, label %else

then:
  %then.value = load i32, ptr %p, align 4
  br label %merge

else:
  %p4 = getelementptr i8, ptr %p, i64 4
  %else.value = load i32, ptr %p4, align 4
  br label %merge

merge:
  %merge.value = load i32, ptr %p, align 4
  %other = load i32, ptr %q, align 4
  %p8 = getelementptr i8, ptr %p, i64 8
  %gap = load i32, ptr %p8, align 4
  store i32 %merge.value, ptr %p, align 4
  ret void
}

define i32 @cross_bb_loop(ptr %p, i32 %n) {
; CHECK-LABEL: mair graph of fn @cross_bb_loop {
; CHECK:       ma1: preds=
; CHECK-NEXT:    read ptr %p, 8
; CHECK:       ma6: preds=
; CHECK-NOT:     read ptr %p, 4
; CHECK:       ma9: preds=
; CHECK-NOT:     read ptr %p + 4, 4
; CHECK:       ma12: preds=
; CHECK-NOT:     read ptr %p, 4
entry:
  %wide = load i64, ptr %p, align 8
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  %head = load i32, ptr %p, align 4
  br label %latch

latch:
  %p4 = getelementptr i8, ptr %p, i64 4
  %tail = load i32, ptr %p4, align 4
  %next = add i32 %i, 1
  br label %header

exit:
  %result = load i32, ptr %p, align 4
  ret i32 %result
}

define void @cross_bb_loop_postdom(ptr %p, i1 %stop) {
; CHECK-LABEL: mair graph of fn @cross_bb_loop_postdom {
; CHECK:       ma5: preds=
; CHECK-NOT:     read ptr %p, 4
; CHECK:       ma8: preds=
; CHECK-NEXT:    read ptr %p, 8
entry:
  br label %header

header:
  br i1 %stop, label %exit, label %body

body:
  %narrow = load i32, ptr %p, align 4
  br label %header

exit:
  %wide = load i64, ptr %p, align 8
  ret void
}

define void @cross_bb_multihop(ptr %p) {
; CHECK-LABEL: mair graph of fn @cross_bb_multihop {
; CHECK:       ma1: preds=
; CHECK-NEXT:    read ptr %p, 8
; CHECK:       ma6: preds=
; CHECK-NOT:     read ptr %p, 4
entry:
  %wide = load i64, ptr %p, align 8
  br label %empty1

empty1:
  br label %empty2

empty2:
  %narrow = load i32, ptr %p, align 4
  ret void
}

define void @cross_bb_dom_postdom_union(ptr %p, ptr %q, i1 %cond) {
; CHECK-LABEL: mair graph of fn @cross_bb_dom_postdom_union {
; CHECK:       ma1: preds=
; CHECK-NEXT:    read ptr %p, 8
; CHECK:       ma4: preds=
; CHECK-NOT:     read ptr %p, 4
; CHECK:       ma5: preds=
; CHECK-NOT:     read ptr %q, 4
; CHECK:       ma10: preds=
; CHECK-NEXT:    read ptr %q, 8
entry:
  %p.wide = load i64, ptr %p, align 8
  br i1 %cond, label %left, label %right

left:
  %p.narrow = load i32, ptr %p, align 4
  %q.narrow = load i32, ptr %q, align 4
  br label %merge

right:
  br label %merge

merge:
  %q.wide = load i64, ptr %q, align 8
  ret void
}

define void @unreachable_component_does_not_flow(ptr %p) {
; CHECK-LABEL: mair graph of fn @unreachable_component_does_not_flow {
; CHECK:         read ptr %p, 8
; CHECK:         read ptr %p, 4
entry:
  %wide = load i64, ptr %p, align 8
  ret void

dead:
  %narrow = load i32, ptr %p, align 4
  ret void
}
