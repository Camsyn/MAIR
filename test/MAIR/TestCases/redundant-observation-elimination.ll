; RUN: mair-opt -passes=roe %s | FileCheck %s
; RUN: mair-opt -passes=roe --disable-output %s
; RUN: mair-opt -passes="simplifygraph,roe" %s | FileCheck %s --check-prefix=INTRA

define void @remove_covered_accesses(ptr %p) {
; CHECK-LABEL: mair graph of fn @remove_covered_accesses {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 8
; CHECK:       pp2: preds=
; CHECK:       pp3: preds=
; CHECK:       pp4: preds=
; CHECK-NEXT:    o: write ptr %p, 4
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
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p + %i, 4
; CHECK:       pp2: preds=
; CHECK-NOT:     o: read ptr %p + %i, 4
entry:
  %pi = getelementptr i8, ptr %p, i64 %i
  %a = load i32, ptr %pi, align 4
  %b = load i32, ptr %pi, align 4
  ret void
}

define void @keep_uncovered_accesses(ptr %p, ptr %q) {
; CHECK-LABEL: mair graph of fn @keep_uncovered_accesses {
; CHECK-NOT:     o: read ptr %p, 4
; CHECK:         o: read ptr %p + 8, 4
; CHECK-NOT:     o: read ptr %p, 4
; CHECK:         o: read ptr %q, 4
; CHECK-NOT:     o: read ptr %p, 4
; CHECK:         o: write ptr %p, 4
; INTRA-LABEL: mair graph of fn @keep_uncovered_accesses {
; INTRA:         pp4: preds=
; INTRA-NOT:       o: read ptr %p, 4
; INTRA-DAG:       o: read ptr %p + 8, 4
; INTRA-DAG:       o: read ptr %q, 4
; INTRA-DAG:       o: write ptr %p, 4
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
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 8
; CHECK:       pp4: preds=
; CHECK-NOT:     o: read ptr %p, 4
; CHECK:       pp7: preds=
; CHECK-NOT:     o: read ptr %p + 4, 4
; CHECK:       pp10: preds=
; CHECK-NOT:     o: read ptr %p, 4
; CHECK:       pp11: preds=
; CHECK-NEXT:    o: read ptr %q, 4
; CHECK:       pp12: preds=
; CHECK-NEXT:    o: read ptr %p + 8, 4
; CHECK:       pp13: preds=
; CHECK-NEXT:    o: write ptr %p, 4
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
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 8
; CHECK:       pp12: preds=
; CHECK-NOT:     o: read ptr %p, 4
; CHECK-LABEL: mair graph of fn @cross_bb_loop of loop %header {
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp9: preds=
; CHECK-NEXT:    o: read ptr %p + 4, 4
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
; CHECK:       pp8: preds=
; CHECK-NEXT:    o: read ptr %p, 8
; CHECK-LABEL: mair graph of fn @cross_bb_loop_postdom of loop %header {
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read ptr %p, 4
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
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 8
; CHECK:       pp6: preds=
; CHECK-NOT:     o: read ptr %p, 4
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
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 8
; CHECK:       pp4: preds=
; CHECK-NOT:     o: read ptr %p, 4
; CHECK:       pp5: preds=
; CHECK-NOT:     o: read ptr %q, 4
; CHECK:       pp10: preds=
; CHECK-NEXT:    o: read ptr %q, 8
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
; CHECK:         o: read ptr %p, 8
; CHECK:         o: read ptr %p, 4
entry:
  %wide = load i64, ptr %p, align 8
  ret void

dead:
  %narrow = load i32, ptr %p, align 4
  ret void
}

define i32 @loop_pointer_conditional(ptr %base, i64 %n) {
; CHECK-LABEL: mair graph of fn @loop_pointer_conditional {
; CHECK:       pp10:
; CHECK-NEXT:    o: read ptr %base + %p, 4
; CHECK-LABEL: mair graph of fn @loop_pointer_conditional of loop %loop {
; CHECK:       pp5:
; CHECK-NEXT:    o: read ptr %base + %p, 4
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %sum = phi i32 [ 0, %entry ], [ %next.sum, %latch ]
  %p = getelementptr i32, ptr %base, i64 %i
  %selected = icmp eq i64 %i, 0
  br i1 %selected, label %body, label %latch
body:
  %v = load i32, ptr %p, align 4
  %added = add i32 %sum, %v
  br label %latch
latch:
  %next.sum = phi i32 [ %sum, %loop ], [ %added, %body ]
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, %n
  br i1 %more, label %loop, label %exit
exit:
  %last = load i32, ptr %p, align 4
  %result = add i32 %last, %next.sum
  ret i32 %result
}

define i32 @same_loop_repeated_read(ptr %base, i64 %n) {
; CHECK-LABEL: mair graph of fn @same_loop_repeated_read {
; CHECK-LABEL: mair graph of fn @same_loop_repeated_read of loop %loop {
; CHECK:       pp3: preds=
; CHECK-NEXT:    o: read ptr %base + %p, 4
; CHECK:       pp4: preds=
; CHECK-NOT:     o: read ptr %base + %p, 4
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr i32, ptr %base, i64 %i
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %p, align 4
  %sum = add i32 %first, %second
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, %n
  br i1 %more, label %loop, label %exit
exit:
  ret i32 %sum
}

; Each pair below covers each other across a diamond: the first access
; dominates the second, which post-dominates it. With equal levels the
; dominator stays.
define i32 @earlier_read_covers_later_read(ptr %p, i1 %c) {
; CHECK-LABEL: mair graph of fn @earlier_read_covers_later_read {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       pp6: preds=
; CHECK-NOT:     o:
; CHECK:       }
entry:
  %a = load i32, ptr %p, align 4
  br i1 %c, label %then, label %join
then:
  br label %join
join:
  %b = load i32, ptr %p, align 4
  %s = add i32 %a, %b
  ret i32 %s
}

define void @earlier_write_covers_later_write(ptr %p, i1 %c) {
; CHECK-LABEL: mair graph of fn @earlier_write_covers_later_write {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: write ptr %p, 4
; CHECK:       pp6: preds=
; CHECK-NOT:     o:
; CHECK:       }
entry:
  store i32 0, ptr %p, align 4
  br i1 %c, label %then, label %join
then:
  br label %join
join:
  store i32 1, ptr %p, align 4
  ret void
}

; A write has a higher level than a read: only the write covers, so it stays
; although it runs second.
define void @later_write_covers_earlier_read(ptr %p, i1 %c) {
; CHECK-LABEL: mair graph of fn @later_write_covers_earlier_read {
; CHECK:       pp1: preds=
; CHECK-NOT:     o:
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: write ptr %p, 4
; CHECK:       }
entry:
  %a = load i32, ptr %p, align 4
  br i1 %c, label %then, label %join
then:
  br label %join
join:
  store i32 %a, ptr %p, align 4
  ret void
}

; The block layout is not a dominance order: %second is laid out before
; %first, which dominates it. The read in %first still stays.
define i32 @dominator_laid_out_later(ptr %p, i1 %c) {
; CHECK-LABEL: mair graph of fn @dominator_laid_out_later {
; CHECK:       pp3: preds=
; CHECK-NOT:     o:
; CHECK:       pp6: preds=
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK:       }
entry:
  br label %first
second:
  %b = load i32, ptr %p, align 4
  ret i32 %b
first:
  %a = load i32, ptr %p, align 4
  br i1 %c, label %then, label %second
then:
  br label %second
}

; p[i]++: the read and the write of one symbolic location share a program
; point after simplifygraph. The write has the higher level and covers the
; read, as for concrete offsets.
define void @same_offset_levels(ptr %p, i64 %i) {
; INTRA-LABEL: mair graph of fn @same_offset_levels {
; INTRA:         pp2: preds=
; INTRA-NEXT:      o: write ptr %p + %a, 4
; INTRA-EMPTY:
  %a = getelementptr i32, ptr %p, i64 %i
  %v = load i32, ptr %a, align 4
  %inc = add i32 %v, 1
  store i32 %inc, ptr %a, align 4
  ret void
}

; Two reads of one symbolic location in one program point: one stays.
define i32 @same_offset_equal_reads(ptr %p, i64 %i) {
; INTRA-LABEL: mair graph of fn @same_offset_equal_reads {
; INTRA:         pp2: preds=
; INTRA-NEXT:      o: read ptr %p + %i, 4
; INTRA-EMPTY:
  %pi = getelementptr i8, ptr %p, i64 %i
  %a = load i32, ptr %pi, align 4
  %b = load i32, ptr %pi, align 4
  %s = add i32 %a, %b
  ret i32 %s
}
