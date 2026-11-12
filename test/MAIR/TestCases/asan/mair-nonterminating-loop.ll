; RUN: env MAIR=1 %clang -asan -O1 -mllvm -asan-opt=false -S -emit-llvm %s -o - | FileCheck %s

; MAIR assumes that cycles terminate. witness post-dominates header, so the
; check in witness covers the load in header and the header keeps none. If
; %repeat stays true the loop spins in header forever and its load is never
; checked. That is accepted: a post-dominator is taken to be reached.
;
; The loads are atomic so that their observations stay at the accesses; the
; only thing that removes the check from header is post-dominance coverage.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @probe(ptr %pointer, i1 %repeat, i1 %finish) sanitize_address {
; CHECK-LABEL: define {{.*}} @probe(
; CHECK:       %early = load atomic i32
; CHECK-NOT:   call void @__asan_report_load4
; CHECK:       witness:
; CHECK:       call void @__asan_report_load4
; CHECK:       %late = load atomic i32
; CHECK-NOT:   call void @__asan_report_load4
; CHECK:       ret i32
entry:
  br label %header

header:
  %early = load atomic i32, ptr %pointer monotonic, align 4
  br i1 %repeat, label %header, label %witness

witness:
  %late = load atomic i32, ptr %pointer monotonic, align 4
  br i1 %finish, label %exit, label %header

exit:
  ret i32 %late
}
