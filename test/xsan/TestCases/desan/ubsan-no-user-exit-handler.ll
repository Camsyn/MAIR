; Reduced from curl urlapi.c set_url.
; A UBSan nonnull handler may be emitted as a no-user-exit block while the
; guarding terminator is not tagged as UBSan. Full UBSan DeSan should leave this
; CFG shape in place because the source terminator is user IR, extract the
; handler call as UBSan add IR, and later ReSan must restore the call.
; RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-de-ubsan -xsan-dump=%t.dump -S %s -o %t.out.ll
; RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/02-desan.ll
; RUN: FileCheck %s --check-prefix=FINAL --input-file=%t.dump/10-final.ll

target triple = "x86_64-unknown-linux-gnu"

@data = external hidden global { { ptr, i32, i32 }, i32, i32 }, no_sanitize_address
@fallback = private unnamed_addr constant [1 x i8] zeroinitializer

define dso_local void @set_url_like(ptr noundef nonnull %u, i1 %guard) sanitize_address sanitize_memory sanitize_thread {
entry:
  %loaded = load ptr, ptr %u, align 8
  br i1 %guard, label %handler.nonnull_arg, label %cont, !prof !1, !nosanitize !0

handler.nonnull_arg:
  call void @__ubsan_handle_nonnull_arg(ptr nonnull @data), !nosanitize !0, !xsan.ubsan !0
  unreachable, !nosanitize !0, !xsan.ubsan !0

cont:
  %value = phi ptr [ %loaded, %entry ]
  call void @use(ptr %value)
  ret void
}

declare void @__ubsan_handle_nonnull_arg(ptr)
declare void @use(ptr)

!0 = !{}
!1 = !{!"branch_weights", i32 1, i32 1048575}

; DESAN-LABEL: define dso_local void @set_url_like(
; DESAN: entry:
; DESAN: br i1 %guard, label %handler.nonnull_arg, label %cont
; DESAN: handler.nonnull_arg:
; DESAN-NEXT: unreachable
; DESAN: cont:
; DESAN-NEXT: %value = phi ptr [ %loaded, %entry ]
; DESAN: call void @use
; DESAN: ret void

; FINAL-LABEL: define dso_local void @set_url_like(
; FINAL: br i1 %guard, label %handler.nonnull_arg, label %cont
; FINAL: handler.nonnull_arg:
; FINAL: __ubsan_handle_nonnull_arg
; FINAL: unreachable
; FINAL: cont:
; FINAL: @use
; FINAL: ret void
