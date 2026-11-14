; Accesses native ASan never checks are dropped before MAIR optimization, so
; they cannot merge with checked ones and drag them back to native per-access
; checks.
;
; RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s
; RUN: env MAIR=1 %clang -asan -O1 -mair-stats -c %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=STATS

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @use(ptr)

@g = global { i32, i32 } zeroinitializer

; Stack safety proves buf[i] in bounds (i < 8), so native ASan does not check
; it, but buf[i + 1] and buf[i + 2] may overflow. Without buf[i], the two
; checked loads merge into one 2-byte range instead of falling back to one
; native check each.
define i8 @stack_tail(i64 %n) sanitize_address {
; CHECK-LABEL: define {{.*}} @stack_tail(
; CHECK-NOT:   call void @__asan_report_load1(
; CHECK:       call void @__asan_report_load_n(i64 %{{.*}}, i64 2)
; CHECK-NOT:   call void @__asan_report_load1(
; CHECK:       ret i8
entry:
  %buf = alloca [8 x i8], align 1
  call void @use(ptr %buf)
  %i = and i64 %n, 7
  %p0 = getelementptr inbounds i8, ptr %buf, i64 %i
  %a = load i8, ptr %p0, align 1
  %p1 = getelementptr inbounds i8, ptr %p0, i64 1
  %b = load i8, ptr %p1, align 1
  %p2 = getelementptr inbounds i8, ptr %p0, i64 2
  %c = load i8, ptr %p2, align 1
  %ab = add i8 %a, %b
  %abc = add i8 %ab, %c
  ret i8 %abc
}

; Both fields of a global are accessed at constant in-bounds offsets, which
; native ASan proves safe. Neither becomes an observation.
define i32 @global_pair() sanitize_address {
; CHECK-LABEL: define {{.*}} @global_pair(
; CHECK-NOT:   call void @__asan_report_load
; CHECK:       ret i32
entry:
  %a = load i32, ptr @g, align 8
  %pb = getelementptr inbounds i8, ptr @g, i64 4
  %b = load i32, ptr %pb, align 4
  %s = add i32 %a, %b
  ret i32 %s
}

; STATS:     MAIR stats
; STATS-NOT: native
; STATS:     select{{ +}}in 1  lowered 1{{$}}
; STATS-NOT: select
