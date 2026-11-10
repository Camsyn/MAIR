; RUN: mair-opt -passes=lpoc %s | FileCheck %s
; RUN: mair-opt -passes=lpoc -emit-llvm %s | FileCheck %s --check-prefix=FLAGS
; RUN: mair-opt -passes=lpoc --disable-output %s

define void @reject_poison_candidate(ptr %base, i64 %offset) {
; CHECK-LABEL: mair graph of fn @reject_poison_candidate {
; CHECK-NOT:   %candidate
; CHECK:       o: read period start=(%base + (2 * %offset)), stride=2, access-size=2, trips=4
; CHECK-NOT:   %candidate
; CHECK:       }
; FLAGS-LABEL: define void @reject_poison_candidate(
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

define void @skip_poison_for_safe_candidate(ptr %base, i64 %offset) {
; CHECK-LABEL: mair graph of fn @skip_poison_for_safe_candidate {
; CHECK:       o: read ptr %base + %safe.candidate, 8
; CHECK-LABEL: mair graph of fn @skip_poison_for_safe_candidate of loop %loop {
; CHECK-NOT:   o:
; CHECK:       }
; FLAGS-LABEL: define void @skip_poison_for_safe_candidate(
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

define void @reject_poison_byte_offset(ptr %base, i64 %offset) {
; CHECK-LABEL: mair graph of fn @reject_poison_byte_offset {
; CHECK-NOT:   %recovered
; CHECK:       o: read period start=(%base + %offset), stride=1, access-size=1, trips=4
; CHECK-NOT:   %recovered
; CHECK:       }
; FLAGS-LABEL: define void @reject_poison_byte_offset(
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
