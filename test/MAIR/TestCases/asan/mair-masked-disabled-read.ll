; With reads disabled, a masked load is not checked. It must not cover the
; masked store after it, or the store loses its check: the load would fall
; back to native lowering, which skips it, and take the store's check along.
;
; RUN: env MAIR=1 %clang -asan -O0 -mllvm -as-instrument-reads=0 -S -emit-llvm \
; RUN:   -x ir %s -o - | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, i32 immarg, <4 x i1>, <4 x i32>)
declare void @llvm.masked.store.v4i32.p0(<4 x i32>, ptr, i32 immarg, <4 x i1>)

define void @f(ptr %p) sanitize_address {
entry:
  %v = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr %p, i32 4, <4 x i1> <i1 true, i1 true, i1 false, i1 false>, <4 x i32> poison)
  br label %next

next:
  %w = add <4 x i32> %v, <i32 1, i32 1, i32 1, i32 1>
  call void @llvm.masked.store.v4i32.p0(<4 x i32> %w, ptr %p, i32 4, <4 x i1> <i1 true, i1 true, i1 false, i1 false>)
  ret void
}

; CHECK-LABEL: define void @f(
; CHECK-NOT: call void @__asan_report_load
; CHECK: call void @__asan_report_store4(
; CHECK: call void @__asan_report_store4(
; CHECK: call void @llvm.masked.store
