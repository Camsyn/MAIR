; RUN: env MAIR=1 %clang_tsan -x ir -S -emit-llvm %s -o - | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-gnu"

declare void @callee()

define i32 @ignored(ptr %p) "sanitize_thread_no_checking_at_run_time" {
entry:
  %value = load i32, ptr %p, align 4
  call void @callee()
  ret i32 %value
}

; CHECK-LABEL: define i32 @ignored
; CHECK: call void @__tsan_func_entry
; CHECK: call void @__tsan_ignore_thread_begin
; CHECK: load i32, ptr %p
; CHECK: invoke void @callee
; CHECK: call void @__tsan_ignore_thread_end
; CHECK: call void @__tsan_func_exit
