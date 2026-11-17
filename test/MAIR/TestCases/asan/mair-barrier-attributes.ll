; Which calls are ASan barriers. A call is not a barrier when it can neither
; synchronize nor free memory (nosync and nofree); a later check then stands in
; for an earlier access only if the call also returns (willreturn and
; nounwind). SanitizerCoverage callbacks are not barriers either. Every other
; call that may write memory is, including the sanitizer interface: the
; container annotations change the shadow.
;
; A loop whose iterations make the call is checked once after its exit only if
; the call is no barrier in either direction.
; RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @ext()
declare void @nosync_nofree_willreturn() nosync nofree willreturn nounwind
declare void @nosync_nofree() nosync nofree nounwind
declare void @nosync_willreturn() nosync willreturn nounwind
declare void @__sanitizer_cov_trace_cmp8(i64, i64)
declare void @__sanitizer_annotate_contiguous_container(ptr, ptr, ptr, ptr)

define i64 @loop_nosync_nofree_willreturn(ptr %p, i64 %n) #0 {
; CHECK-LABEL: define {{.*}} @loop_nosync_nofree_willreturn(
; CHECK-NOT: call void @__asan_report_load8
; CHECK: call void @__asan_loadN(
; CHECK-NOT: call void @__asan_report_load8
; CHECK: {{^}$}}
entry:
  %guard = icmp sgt i64 %n, 0
  br i1 %guard, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  %addr = getelementptr inbounds i64, ptr %p, i64 %i
  %v = load i64, ptr %addr, align 8
  %s.next = add i64 %s, %v
  call void @nosync_nofree_willreturn()
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  ret i64 %r
}

define i64 @loop_sancov(ptr %p, i64 %n, i64 %k) #0 {
; CHECK-LABEL: define {{.*}} @loop_sancov(
; CHECK-NOT: call void @__asan_report_load8
; CHECK: call void @__asan_loadN(
; CHECK-NOT: call void @__asan_report_load8
; CHECK: {{^}$}}
entry:
  %guard = icmp sgt i64 %n, 0
  br i1 %guard, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  %addr = getelementptr inbounds i64, ptr %p, i64 %i
  %v = load i64, ptr %addr, align 8
  call void @__sanitizer_cov_trace_cmp8(i64 %v, i64 %k)
  %s.next = add i64 %s, %v
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  ret i64 %r
}

; The call may not return, so the exit is not certain to run after an
; iteration: the loop is not summarized.
define i64 @loop_nosync_nofree(ptr %p, i64 %n) #0 {
; CHECK-LABEL: define {{.*}} @loop_nosync_nofree(
; CHECK-NOT: call void @__asan_loadN(
; CHECK: call void @__asan_report_load8(
; CHECK: call void @nosync_nofree()
; CHECK-NOT: call void @__asan_loadN(
; CHECK: {{^}$}}
entry:
  %guard = icmp sgt i64 %n, 0
  br i1 %guard, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  %addr = getelementptr inbounds i64, ptr %p, i64 %i
  %v = load i64, ptr %addr, align 8
  %s.next = add i64 %s, %v
  call void @nosync_nofree()
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  ret i64 %r
}

; The call may free memory, which TSan marks as freed.
define i64 @loop_nosync_willreturn(ptr %p, i64 %n) #0 {
; CHECK-LABEL: define {{.*}} @loop_nosync_willreturn(
; CHECK-NOT: call void @__asan_loadN(
; CHECK: call void @__asan_report_load8(
; CHECK: call void @nosync_willreturn()
; CHECK-NOT: call void @__asan_loadN(
; CHECK: {{^}$}}
entry:
  %guard = icmp sgt i64 %n, 0
  br i1 %guard, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  %addr = getelementptr inbounds i64, ptr %p, i64 %i
  %v = load i64, ptr %addr, align 8
  %s.next = add i64 %s, %v
  call void @nosync_willreturn()
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  ret i64 %r
}

define i64 @loop_ext(ptr %p, i64 %n) #0 {
; CHECK-LABEL: define {{.*}} @loop_ext(
; CHECK-NOT: call void @__asan_loadN(
; CHECK: call void @__asan_report_load8(
; CHECK: call void @ext()
; CHECK-NOT: call void @__asan_loadN(
; CHECK: {{^}$}}
entry:
  %guard = icmp sgt i64 %n, 0
  br i1 %guard, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  %addr = getelementptr inbounds i64, ptr %p, i64 %i
  %v = load i64, ptr %addr, align 8
  %s.next = add i64 %s, %v
  call void @ext()
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  ret i64 %r
}

; Only the SanitizerCoverage callbacks are let through: a container annotation
; poisons or unpoisons shadow.
define i64 @loop_annotate(ptr %p, i64 %n, ptr %m) #0 {
; CHECK-LABEL: define {{.*}} @loop_annotate(
; CHECK-NOT: call void @__asan_loadN(
; CHECK: call void @__asan_report_load8(
; CHECK: call void @__sanitizer_annotate_contiguous_container(
; CHECK-NOT: call void @__asan_loadN(
; CHECK: {{^}$}}
entry:
  %guard = icmp sgt i64 %n, 0
  br i1 %guard, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  %addr = getelementptr inbounds i64, ptr %p, i64 %i
  %v = load i64, ptr %addr, align 8
  %s.next = add i64 %s, %v
  call void @__sanitizer_annotate_contiguous_container(ptr %m, ptr %m, ptr %m, ptr %m)
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  ret i64 %r
}

; Straight-line code: a call that may not return still lets an earlier check
; stand in for a later access of the same bytes.
define i64 @forward_nosync_nofree(ptr %p) #0 {
; CHECK-LABEL: define {{.*}} @forward_nosync_nofree(
; CHECK: call void @__asan_report_load8(
; CHECK-NOT: call void @__asan_report_load
; CHECK: call void @nosync_nofree()
; CHECK-NOT: call void @__asan_report_load
; CHECK: ret i64
entry:
  %a = load i64, ptr %p, align 8
  call void @nosync_nofree()
  %b = load i64, ptr %p, align 8
  %r = add i64 %a, %b
  ret i64 %r
}

; But a later check cannot stand in for an earlier access across that call:
; the 4-byte check stays.
define i64 @backward_nosync_nofree(ptr %p) #0 {
; CHECK-LABEL: define {{.*}} @backward_nosync_nofree(
; CHECK: call void @__asan_report_load4(
; CHECK: call void @nosync_nofree()
; CHECK: call void @__asan_report_load8(
; CHECK: ret i64
entry:
  %hi = getelementptr inbounds i8, ptr %p, i64 4
  %a = load i32, ptr %hi, align 4
  call void @nosync_nofree()
  %b = load i64, ptr %p, align 8
  %a.ext = zext i32 %a to i64
  %r = add i64 %a.ext, %b
  ret i64 %r
}

; It can when the call returns.
define i64 @backward_nosync_nofree_willreturn(ptr %p) #0 {
; CHECK-LABEL: define {{.*}} @backward_nosync_nofree_willreturn(
; CHECK-NOT: call void @__asan_report_load4(
; CHECK: call void @__asan_report_load8(
; CHECK-NOT: call void @__asan_report_load4(
; CHECK: ret i64
entry:
  %hi = getelementptr inbounds i8, ptr %p, i64 4
  %a = load i32, ptr %hi, align 4
  call void @nosync_nofree_willreturn()
  %b = load i64, ptr %p, align 8
  %a.ext = zext i32 %a to i64
  %r = add i64 %a.ext, %b
  ret i64 %r
}

attributes #0 = { nounwind sanitize_address }
