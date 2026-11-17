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

; Coverage of an access a constant distance from a symbolic anchor. Both
; addresses are SCEVs of the same values, and ScalarEvolution proves the
; distance (computeConstantDifference).

; p is an array of 4-byte elements: the 8-byte write at p[i] covers the read
; of p[i+1], in one program point too.
define void @same_block_levels(ptr %p, i64 %i) {
; CHECK-LABEL: mair graph of fn @same_block_levels {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: write ptr %p + %a, 8
; CHECK:       pp2: preds=
; CHECK-EMPTY:
; INTRA-LABEL: mair graph of fn @same_block_levels {
; INTRA:         pp2: preds=
; INTRA-NEXT:      o: write ptr %p + %a, 8
; INTRA-EMPTY:
  %a = getelementptr [4 x i8], ptr %p, i64 %i
  store i64 0, ptr %a, align 4
  %i1 = add nsw i64 %i, 1
  %b = getelementptr [4 x i8], ptr %p, i64 %i1
  %w = load i32, ptr %b, align 4
  ret void
}

; One symbolic offset: the wider read covers the narrower one.
define void @same_symbol_narrower(ptr %p, i64 %i) {
; CHECK-LABEL: mair graph of fn @same_symbol_narrower {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p + %i, 8
; CHECK:       pp4: preds=
; CHECK-EMPTY:
  %a = getelementptr i8, ptr %p, i64 %i
  %v = load i64, ptr %a, align 8
  br label %next
next:
  %w = load i32, ptr %a, align 4
  ret void
}

; The second field of a 16-byte struct, read in a conditional block, lies in
; the 16 bytes read before the branch.
define void @struct_cross_block(ptr %s, i64 %i, i1 %c) {
; CHECK-LABEL: mair graph of fn @struct_cross_block {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %s + %base, 16
; CHECK:       pp4: preds=
; CHECK-EMPTY:
  %base = getelementptr [16 x i8], ptr %s, i64 %i
  %y = load i128, ptr %base, align 8
  br i1 %c, label %t, label %e
t:
  %f2 = getelementptr i8, ptr %base, i64 8
  %z = load i64, ptr %f2, align 8
  br label %e
e:
  ret void
}

; p[i+1] of a 4-byte array lies in the 8 bytes at p[i].
define void @scaled_cross_block(ptr %p, i64 %i, i1 %c) {
; CHECK-LABEL: mair graph of fn @scaled_cross_block {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p + %a, 8
; CHECK:       pp4: preds=
; CHECK-EMPTY:
  %a = getelementptr [4 x i8], ptr %p, i64 %i
  %v = load i64, ptr %a, align 4
  br i1 %c, label %t, label %e
t:
  %i1 = add nsw i64 %i, 1
  %b = getelementptr [4 x i8], ptr %p, i64 %i1
  %w = load i32, ptr %b, align 4
  br label %e
e:
  ret void
}

; Two GEPs compute the same address.
define void @two_equal_geps(ptr %p, i64 %i) {
; CHECK-LABEL: mair graph of fn @two_equal_geps {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: write ptr %p + %a, 4
; CHECK:       pp4: preds=
; CHECK-EMPTY:
  %a = getelementptr i32, ptr %p, i64 %i
  store i32 0, ptr %a, align 4
  br label %next
next:
  %b = getelementptr i32, ptr %p, i64 %i
  %w = load i32, ptr %b, align 4
  ret void
}

; p[i-1] starts before p[i], and the 8 bytes at p[i+1] end after the 8 bytes
; at p[i]: neither is covered.
define void @negative_delta_and_oversize(ptr %p, i64 %i, i1 %c) {
; CHECK-LABEL: mair graph of fn @negative_delta_and_oversize {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p + %a, 8
; CHECK:       pp4: preds=
; CHECK-NEXT:    o: read ptr %p + %b, 4
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read ptr %p + %c2, 8
  %a = getelementptr [4 x i8], ptr %p, i64 %i
  %v = load i64, ptr %a, align 4
  br i1 %c, label %t, label %e
t:
  %im1 = add nsw i64 %i, -1
  %b = getelementptr [4 x i8], ptr %p, i64 %im1
  %w = load i32, ptr %b, align 4
  %i2 = add nsw i64 %i, 1
  %c2 = getelementptr [4 x i8], ptr %p, i64 %i2
  %x = load i64, ptr %c2, align 4
  br label %e
e:
  ret void
}

; No constant distance: p[j] is unrelated to p[i], and i + 1 computed in 32
; bits without nsw may wrap before it is extended.
define void @no_constant_delta(ptr %p, i32 %i, i64 %j, i1 %c) {
; CHECK-LABEL: mair graph of fn @no_constant_delta {
; CHECK:       pp1: preds=
; CHECK-NEXT:    o: read ptr %p + %a, 8
; CHECK:       pp4: preds=
; CHECK-NEXT:    o: read ptr %p + %b, 4
; CHECK:       pp5: preds=
; CHECK-NEXT:    o: read ptr %p + %c1, 4
  %ie = sext i32 %i to i64
  %a = getelementptr [4 x i8], ptr %p, i64 %ie
  %v = load i64, ptr %a, align 4
  br i1 %c, label %t, label %e
t:
  %b = getelementptr [4 x i8], ptr %p, i64 %j
  %w = load i32, ptr %b, align 4
  %i1 = add i32 %i, 1
  %i1e = sext i32 %i1 to i64
  %c1 = getelementptr [4 x i8], ptr %p, i64 %i1e
  %x = load i32, ptr %c1, align 4
  br label %e
e:
  ret void
}

; B post-dominates A only around the back edge. For i < 3 A runs and B does
; not in that iteration, so B's read of p[i] does not cover A's read of
; p[i+1]: the edge is closed to A, whose address changes every iteration.
define void @iterated_pdom(ptr %p) {
; CHECK-LABEL: mair graph of fn @iterated_pdom of loop %H {
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: read ptr %p + %a, 8
; CHECK:       pp10: preds=
; CHECK-NEXT:    o: read ptr %p + %b, 4
entry:
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %A ]
  %small = icmp ult i64 %i, 3
  %a = getelementptr [4 x i8], ptr %p, i64 %i
  %i1 = add nsw i64 %i, 1
  %b = getelementptr [4 x i8], ptr %p, i64 %i1
  br i1 %small, label %X, label %B
X:
  br label %A
B:
  %v = load i64, ptr %a, align 4
  %d = icmp eq i64 %i, 100
  br i1 %d, label %exit, label %A
A:
  %w = load i32, ptr %b, align 4
  %i.next = add nsw i64 %i, 1
  br label %H
exit:
  ret void
}

; A reads p[k], which is invariant in the loop, so the iterated edge is open
; to it. B reads the 8 bytes at p[k-1], which cover p[k], through a GEP
; computed in the loop. The address is the same in every iteration, but a
; witness must be invariant in every cycle its target is, so B does not
; cover A. B does not exit the loop: LoopSimplify hoists the leading invariant
; instructions of an exiting block to the preheader.
define void @iterated_pdom_invariant(ptr %p, i64 %k) {
; CHECK-LABEL: mair graph of fn @iterated_pdom_invariant of loop %H {
; CHECK:       pp7: preds=
; CHECK-NEXT:    o: read ptr %p + %a, 8
; CHECK:       pp12: preds=
; CHECK-NEXT:    o: read ptr %p + %t, 4
entry:
  %t = getelementptr [4 x i8], ptr %p, i64 %k
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %A ]
  %small = icmp ult i64 %i, 3
  br i1 %small, label %X, label %B
X:
  br label %A
B:
  %k1 = add nsw i64 %k, -1
  %a = getelementptr [4 x i8], ptr %p, i64 %k1
  %v = load i64, ptr %a, align 4
  br label %latch
latch:
  %d = icmp eq i64 %i, 100
  br i1 %d, label %exit, label %A
A:
  %w = load i32, ptr %t, align 4
  %i.next = add nsw i64 %i, 1
  br label %H
exit:
  ret void
}
