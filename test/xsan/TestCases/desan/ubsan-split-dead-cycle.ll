; Reduced from iproute2 misc/ssfilter.tab.c yyparse.
; UBSan split extraction can leave a dead use-def cycle around a split PHI:
;   %split -> %yyvsp.4 -> %add.ptr578 -> %yyvsp.4
; DeSan's logical DCE must not process already-dead instructions repeatedly.
; RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-de-ubsan -xsan-dump=%t.dump -S %s -o %t.out.ll
; RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/02-desan.ll
; RUN: FileCheck %s --check-prefix=FINAL --input-file=%t.dump/10-final.ll

target triple = "x86_64-unknown-linux-gnu"

@data = private global i32 0, align 4
@table = private global [8 x i8] zeroinitializer, align 8

define void @yyparse_like(i1 %c0, i1 %c1, i1 %c2, ptr %base, ptr %stack) sanitize_address sanitize_memory sanitize_thread {
entry:
  br label %for.cond

for.cond:
  %yyvsp.4 = phi ptr [ %base, %entry ], [ %add.ptr578, %cont585 ], !xsan.ubsan !0
  %yyssp.4 = phi ptr [ %stack, %entry ], [ %add.ptr581, %cont585 ]
  br i1 %c0, label %if.end564, label %handler.out_of_bounds518, !prof !1, !nosanitize !0, !xsan.ubsan !0

handler.out_of_bounds518:
  call void @__ubsan_handle_out_of_bounds(ptr @data, i64 0), !nosanitize !0, !xsan.ubsan !0
  br label %if.end564, !nosanitize !0, !xsan.ubsan !0

if.end564:
  br i1 %c1, label %xsan.loop.exit, label %if.end568, !nosanitize !0, !xsan.ubsan !0

xsan.loop.exit:
  %split = phi ptr [ %yyvsp.4, %if.end564 ], !xsan.ubsan !0
  br label %yyreturnlab, !nosanitize !0, !xsan.ubsan !0

if.end568:
  br i1 %c0, label %cont570, label %handler.out_of_bounds569, !prof !1, !nosanitize !0, !xsan.ubsan !0

handler.out_of_bounds569:
  call void @__ubsan_handle_out_of_bounds(ptr @data, i64 1), !nosanitize !0, !xsan.ubsan !0
  br label %cont570, !nosanitize !0, !xsan.ubsan !0

cont570:
  br i1 %c2, label %cont576, label %handler.pointer_overflow573, !prof !1, !nosanitize !0, !xsan.ubsan !0

handler.pointer_overflow573:
  call void @__ubsan_handle_pointer_overflow(ptr @data, i64 0, i64 1), !nosanitize !0, !xsan.ubsan !0
  br label %cont576, !nosanitize !0, !xsan.ubsan !0

cont576:
  %add.ptr578 = getelementptr inbounds i8, ptr %yyvsp.4, i64 -8, !xsan.ubsan !0
  br i1 %c2, label %cont580, label %handler.pointer_overflow579, !prof !1, !nosanitize !0, !xsan.ubsan !0

handler.pointer_overflow579:
  call void @__ubsan_handle_pointer_overflow(ptr @data, i64 2, i64 3), !nosanitize !0, !xsan.ubsan !0
  br label %cont580, !nosanitize !0, !xsan.ubsan !0

cont580:
  %add.ptr581 = getelementptr inbounds i8, ptr %yyssp.4, i64 -1
  br i1 %c2, label %cont585, label %handler.pointer_overflow582, !prof !1, !nosanitize !0, !xsan.ubsan !0

handler.pointer_overflow582:
  call void @__ubsan_handle_pointer_overflow(ptr @data, i64 4, i64 5), !nosanitize !0, !xsan.ubsan !0
  br label %cont585, !nosanitize !0, !xsan.ubsan !0

cont585:
  %loaded = load i8, ptr %add.ptr581, align 1
  br label %for.cond

yyreturnlab:
  %yyvsp.6.ph = phi ptr [ %split, %xsan.loop.exit ], !xsan.ubsan !0
  call void @use(ptr %base)
  ret void
}

declare void @__ubsan_handle_out_of_bounds(ptr, i64)
declare void @__ubsan_handle_pointer_overflow(ptr, i64, i64)
declare void @use(ptr)

!0 = !{}
!1 = !{!"branch_weights", i32 1048575, i32 1}

; DESAN-LABEL: define void @yyparse_like(
; DESAN-NOT: <badref>
; DESAN: br i1 %c1, label %xsan.loop.exit, label %if.end568
; DESAN: %add.ptr578 =
; DESAN: yyreturnlab:
; DESAN: call void @use
; DESAN: ret void

; FINAL-LABEL: define void @yyparse_like(
; FINAL: __ubsan_handle_pointer_overflow
; FINAL: @use
; FINAL: ret void
