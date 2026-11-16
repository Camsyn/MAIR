; A fixed vector load or store is one access of its store size, as native TSan
; records it, so it merges with scalar accesses and lowers through MAIR. A
; vtable pointer access keeps native TSan's __tsan_vptr_* lowering, which
; reports a race on it as a race on the vptr: it takes no part in MAIR
; optimization, so it neither covers nor merges with a plain access.
;
; RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
; RUN: env MAIR=1 %clang -tsan -O1 -mair-stats -c %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=STATS

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; 8 bytes at an 8-aligned start: the aligned 8-byte entry.
define <2 x i32> @vec8(ptr %p) sanitize_thread {
; CHECK-LABEL: define {{.*}} @vec8(
; CHECK:       call void @__tsan_read8(ptr %p)
; CHECK:       ret <2 x i32>
entry:
  %v = load <2 x i32>, ptr %p, align 8
  ret <2 x i32> %v
}

; 16 bytes whose start is only 4-aligned may span three shadow cells.
define void @vec16_unaligned(ptr %p) sanitize_thread {
; CHECK-LABEL: define {{.*}} @vec16_unaligned(
; CHECK:       call void @__tsan_unaligned_write16(ptr %p)
; CHECK:       ret void
entry:
  store <4 x i32> zeroinitializer, ptr %p, align 4
  ret void
}

; Widths with no fixed-size entry, which native TSan leaves unrecorded, are
; recorded as a range of their bytes.
define <8 x i32> @vec32(ptr %p) sanitize_thread {
; CHECK-LABEL: define {{.*}} @vec32(
; CHECK:       call void @__tsan_read_range(ptr %p, i64 32)
; CHECK:       ret <8 x i32>
entry:
  %v = load <8 x i32>, ptr %p, align 32
  ret <8 x i32> %v
}

define <3 x i32> @vec12(ptr %p) sanitize_thread {
; CHECK-LABEL: define {{.*}} @vec12(
; CHECK:       call void @__tsan_read_range(ptr %p, i64 12)
; CHECK:       ret <3 x i32>
entry:
  %v = load <3 x i32>, ptr %p, align 4
  ret <3 x i32> %v
}

; A vector store and the scalar store right after it merge into one 16-byte
; write at an 8-aligned start.
define void @vec_scalar_merged(ptr %p, <2 x i32> %v, i64 %x) sanitize_thread {
; CHECK-LABEL: define {{.*}} @vec_scalar_merged(
; CHECK-NOT:   call void @__tsan_write8(
; CHECK:       call void @__tsan_write16(ptr %p)
; CHECK-NOT:   call void @__tsan_write
; CHECK:       ret void
entry:
  store <2 x i32> %v, ptr %p, align 8
  %q = getelementptr inbounds i8, ptr %p, i64 8
  store i64 %x, ptr %q, align 8
  ret void
}

; The vptr load does not cover the plain load of the same bytes, and does not
; merge with the load of the field after it.
define i64 @vptr_then_fields(ptr %obj) sanitize_thread {
; CHECK-LABEL: define {{.*}} @vptr_then_fields(
; CHECK:       call void @__tsan_vptr_read(ptr %obj)
; CHECK:       call void @__tsan_read16(ptr %obj)
; CHECK-NOT:   call void @__tsan_{{read|write|unaligned|vptr}}
; CHECK:       ret i64
entry:
  %vptr = load ptr, ptr %obj, align 8, !tbaa !0
  call void @use(ptr %vptr)
  %raw = load i64, ptr %obj, align 8
  %f = getelementptr inbounds i8, ptr %obj, i64 8
  %field = load i64, ptr %f, align 8
  %sum = add i64 %raw, %field
  ret i64 %sum
}

; Storing two vptrs at once updates the first one, as native TSan does.
define void @vptr_vector_store(ptr %obj, <2 x ptr> %vtables) sanitize_thread {
; CHECK-LABEL: define {{.*}} @vptr_vector_store(
; CHECK:       %[[FIRST:.*]] = extractelement <2 x ptr> %vtables, i32 0
; CHECK:       call void @__tsan_vptr_update(ptr %obj, ptr %[[FIRST]])
; CHECK-NOT:   call void @__tsan_{{read|write|unaligned|vptr}}
; CHECK:       ret void
entry:
  store <2 x ptr> %vtables, ptr %obj, align 8, !tbaa !0
  ret void
}

declare void @use(ptr)

!0 = !{!1, !1, i64 0}
!1 = !{!"vtable pointer", !2, i64 0}
!2 = !{!"Simple C++ TBAA"}

; Neither a vector nor a merged source falls back to native lowering.
; STATS:     MAIR stats
; STATS:     select{{ +}}in 6  lowered 6{{$}}
