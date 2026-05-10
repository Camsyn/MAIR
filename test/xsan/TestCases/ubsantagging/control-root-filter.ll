; RUN: %xsan_opt -passes=xsan-ubsan-tagging -S %s -o - | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

@data = private unnamed_addr global i8 0, align 1

define void @no_prof_conditional_predecessor(i1 %cond, i64 %value) {
; CHECK-LABEL: define void @no_prof_conditional_predecessor(
; CHECK:       entry:
; CHECK-NEXT:    br i1 %cond, label %cont, label %handler, !nosanitize !0
; CHECK:       handler:
; CHECK-NEXT:    call void @__ubsan_handle_type_mismatch_v1(ptr @data, i64 %value), !nosanitize !0, !xsan.ubsan !0
; CHECK-NEXT:    br label %cont, !xsan.ubsan !0
;
entry:
  br i1 %cond, label %cont, label %handler, !nosanitize !0

handler:
  call void @__ubsan_handle_type_mismatch_v1(ptr @data, i64 %value), !nosanitize !0
  br label %cont

cont:
  ret void
}

; Exceptional Case
define void @conditional_handler_terminator(i1 %cond, i1 %handler.cond, i64 %value) {
; CHECK-LABEL: define void @conditional_handler_terminator(
; CHECK:       entry:
; CHECK-NEXT:    br i1 %cond, label %cont, label %handler, !prof !1, !nosanitize !0
; CHECK:       handler:
; CHECK-NEXT:    call void @__ubsan_handle_type_mismatch_v1(ptr @data, i64 %value), !nosanitize !0, !xsan.ubsan !0
; CHECK-NEXT:    br i1 %handler.cond, label %cont, label %alt{{$}}
;
entry:
  br i1 %cond, label %cont, label %handler, !prof !1, !nosanitize !0

handler:
  call void @__ubsan_handle_type_mismatch_v1(ptr @data, i64 %value), !nosanitize !0
  br i1 %handler.cond, label %cont, label %alt

alt:
  ret void

cont:
  ret void
}

declare void @__ubsan_handle_type_mismatch_v1(ptr, i64)

!0 = !{}
!1 = !{!"branch_weights", i32 1048575, i32 1}
