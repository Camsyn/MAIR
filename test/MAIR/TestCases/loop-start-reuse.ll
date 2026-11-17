; RUN: mair-opt -passes=lpoc %s | FileCheck %s
; RUN: mair-opt -passes=lpoc -emit-llvm %s | FileCheck %s --check-prefix=FLAGS
; RUN: mair-opt -passes=lpoc --disable-output %s

; %candidate has the SCEV of the loop start, but the nsw on %poison.sum makes
; it poison for most %offset where the start is not. The observation checks
; the address the program computes either way, so it reuses %candidate (as
; native ASan would check a poison pointer). The IR keeps the flag: the
; optimization does not change the IR.
define void @reuse_candidate_with_poison_flag(ptr %base, i64 %offset) {
; CHECK-LABEL: mair graph of fn @reuse_candidate_with_poison_flag {
; CHECK:       o: read ptr %base + %candidate, 8
; CHECK-LABEL: mair graph of fn @reuse_candidate_with_poison_flag of loop %loop {
; CHECK-NOT:   o:
; CHECK:       }
; FLAGS-LABEL: define void @reuse_candidate_with_poison_flag(
; FLAGS:       %poison.sum = add nsw i64 %offset, 9223372036854775807
entry:
  %poison.sum = add nsw i64 %offset, 9223372036854775807
  %recovered = add i64 %poison.sum, -9223372036854775807
  %candidate = getelementptr i16, ptr %base, i64 %recovered
  br label %loop
loop:
  %iteration = phi i64 [ 0, %entry ], [ %next, %loop ]
  %index = add i64 %offset, %iteration
  %address = getelementptr i16, ptr %base, i64 %index
  %value = load i16, ptr %address, align 1
  %next = add nuw i64 %iteration, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

declare void @unrelated() nounwind willreturn memory(none)

define void @reuse_safe_candidate(ptr %base, i64 %offset, float %scalar, <2 x i64> %vector, { i64, i64 } %aggregate) {
; CHECK-LABEL: mair graph of fn @reuse_safe_candidate {
; CHECK:       o: read ptr %base + %candidate, 8
; CHECK-LABEL: mair graph of fn @reuse_safe_candidate of loop %loop {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  call void @unrelated()
  %scalar.result = fadd float %scalar, 1.0
  %vector.result = add <2 x i64> %vector, <i64 1, i64 1>
  %aggregate.result = insertvalue { i64, i64 } %aggregate, i64 %offset, 0
  br label %preheader
preheader:
  %candidate = getelementptr i16, ptr %base, i64 %offset
  br label %loop
loop:
  %iteration = phi i64 [ 0, %preheader ], [ %next, %loop ]
  %index = add i64 %offset, %iteration
  %address = getelementptr i16, ptr %base, i64 %index
  %value = load i16, ptr %address, align 1
  %next = add nuw i64 %iteration, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; Poison flags do not rank candidates: the first one in the function wins.
define void @first_candidate_despite_poison_flag(ptr %base, i64 %offset) {
; CHECK-LABEL: mair graph of fn @first_candidate_despite_poison_flag {
; CHECK:       o: read ptr %base + %poison.candidate, 8
; CHECK-LABEL: mair graph of fn @first_candidate_despite_poison_flag of loop %loop {
; CHECK-NOT:   o:
; CHECK:       }
; FLAGS-LABEL: define void @first_candidate_despite_poison_flag(
; FLAGS:       %poison.sum = add nsw i64 %offset, 9223372036854775807
entry:
  %poison.sum = add nsw i64 %offset, 9223372036854775807
  %recovered = add i64 %poison.sum, -9223372036854775807
  %poison.candidate = getelementptr i16, ptr %base, i64 %recovered
  %safe.candidate = getelementptr i16, ptr %base, i64 %offset
  br label %loop
loop:
  %iteration = phi i64 [ 0, %entry ], [ %next, %loop ]
  %index = add i64 %offset, %iteration
  %address = getelementptr i16, ptr %base, i64 %index
  %value = load i16, ptr %address, align 1
  %next = add nuw i64 %iteration, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

define void @reuse_byte_offset_with_poison_flag(ptr %base, i64 %offset) {
; CHECK-LABEL: mair graph of fn @reuse_byte_offset_with_poison_flag {
; CHECK:       o: read ptr %base + %recovered, 4
; CHECK-LABEL: mair graph of fn @reuse_byte_offset_with_poison_flag of loop %loop {
; CHECK-NOT:   o:
; CHECK:       }
; FLAGS-LABEL: define void @reuse_byte_offset_with_poison_flag(
; FLAGS:       %poison.sum = add nsw i64 %offset, 9223372036854775807
entry:
  %poison.sum = add nsw i64 %offset, 9223372036854775807
  %recovered = add i64 %poison.sum, -9223372036854775807
  %candidate = getelementptr i8, ptr %base, i64 %recovered
  br label %loop
loop:
  %iteration = phi i64 [ 0, %entry ], [ %next, %loop ]
  %index = add i64 %offset, %iteration
  %address = getelementptr i8, ptr %base, i64 %index
  %value = load i8, ptr %address, align 1
  %next = add nuw i64 %iteration, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; A disjoint or is an add only while its operands share no bit: otherwise
; %candidate computes another address than the start, so it is not reused.
define void @reject_disjoint_candidate(ptr %base, i64 %offset) {
; CHECK-LABEL: mair graph of fn @reject_disjoint_candidate {
; CHECK-NOT:   %candidate
; CHECK:       o: read period start=(%base + (2 * %offset)), stride=2, access-size=2, trips=4
; CHECK-NOT:   %candidate
; CHECK:       }
; FLAGS-LABEL: define void @reject_disjoint_candidate(
; FLAGS:       %poison.sum = or disjoint i64 %offset, 1
entry:
  %poison.sum = or disjoint i64 %offset, 1
  %recovered = add i64 %poison.sum, -1
  %candidate = getelementptr i16, ptr %base, i64 %recovered
  br label %loop
loop:
  %iteration = phi i64 [ 0, %entry ], [ %next, %loop ]
  %index = add i64 %offset, %iteration
  %address = getelementptr i16, ptr %base, i64 %index
  %value = load i16, ptr %address, align 1
  %next = add nuw i64 %iteration, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
