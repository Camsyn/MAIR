; Reduced from:
;   llvm-extract 00-raw.ll --func=_ZN10SAXParserP9parseNameEv
; The old single RPO pass visited while.cond.preheader before
; while.cond.sink.split became a user entry and then never revisited it.
; RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-de-ubsan -xsan-dump=%t.dump -S %s -o %t.out.ll
; RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/02-desan.ll

target triple = "x86_64-unknown-linux-gnu"

@ubsan_data_if_then = external hidden unnamed_addr global { { ptr, i32, i32 }, ptr, i8, i8 }, no_sanitize_address
@ubsan_data_loop = external hidden unnamed_addr global { { ptr, i32, i32 }, ptr, i8, i8 }, no_sanitize_address

define dso_local noundef ptr @_ZN10SAXParserP9parseNameEv(ptr noundef nonnull align 8 dereferenceable(52) %this, ptr %call, i1 %is_name_start, i1 %align_ok, i1 %loop_ok) sanitize_address sanitize_memory sanitize_thread {
entry:
  %0 = ptrtoint ptr %call to i64
  br i1 %is_name_start, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  br i1 %align_ok, label %while.cond.preheader, label %while.cond.sink.split, !prof !1, !nosanitize !0

while.cond.sink.split:                            ; preds = %handler.type_mismatch50, %if.then
  %.sink = phi ptr [ @ubsan_data_loop, %handler.type_mismatch50 ], [ @ubsan_data_if_then, %if.then ]
  tail call void @__ubsan_handle_type_mismatch_v1(ptr nonnull %.sink, i64 %0), !nosanitize !0
  br label %while.cond.preheader

while.cond.preheader:                             ; preds = %while.cond.sink.split, %if.then
  br label %while.cond

while.cond:                                       ; preds = %while.body, %while.cond.preheader
  %curch = getelementptr inbounds i8, ptr %this, i64 40
  %curch.value = load i32, ptr %curch, align 8
  %conv57 = trunc i32 %curch.value to i8
  %call58 = tail call noundef ptr @_ZN13StringBuilder6appendEc(ptr noundef nonnull align 8 dereferenceable(24) %call, i8 noundef signext %conv57)
  tail call void @_ZN10SAXParserP6nextchEv(ptr noundef nonnull align 8 dereferenceable(52) %this)
  br i1 %loop_ok, label %while.body, label %if.end

while.body:                                       ; preds = %while.cond
  br i1 %align_ok, label %while.cond, label %handler.type_mismatch50, !prof !1, !llvm.loop !2, !nosanitize !0

handler.type_mismatch50:                          ; preds = %while.body
  br label %while.cond.sink.split, !llvm.loop !2, !nosanitize !0

if.end:                                           ; preds = %while.cond, %entry
  ret ptr %call
}

declare void @__ubsan_handle_type_mismatch_v1(ptr, i64)
declare dso_local void @_ZN10SAXParserP6nextchEv(ptr noundef nonnull align 8 dereferenceable(52)) local_unnamed_addr
declare noundef ptr @_ZN13StringBuilder6appendEc(ptr noundef nonnull align 8 dereferenceable(24), i8 noundef signext) local_unnamed_addr

!0 = !{}
!1 = !{!"branch_weights", i32 1048575, i32 1}
!2 = distinct !{!2, !3}
!3 = !{!"llvm.loop.mustprogress"}

; DESAN-LABEL: define dso_local noundef ptr @_ZN10SAXParserP9parseNameEv(
; DESAN: if.then:
; DESAN-NEXT: br i1 {{.*}}, label %while.cond.preheader, label %while.cond.sink.split
; DESAN: while.cond.sink.split:{{.*}}; preds = {{.*}}%handler.type_mismatch50{{.*}}%if.then
; DESAN: br label %while.cond.preheader
; DESAN: while.cond.preheader:{{.*}}; preds = {{.*}}%while.cond.sink.split{{.*}}%if.then
; DESAN-NEXT: br label %while.cond
; DESAN-NOT: while.cond.preheader:{{[ ]+}}; preds = %if.then, %__xsan_desan_add_record_bb
; DESAN: while.cond:
; DESAN: ret ptr %call
