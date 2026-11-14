; Accesses native TSan never records are dropped before MAIR optimization, so
; they cannot merge with recorded ones and drag them back to native per-access
; instrumentation.
;
; RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s
; RUN: env MAIR=1 %clang -tsan -O1 -mair-stats -c %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=STATS

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @use(ptr)

@table = constant [8 x i8] c"\00\01\02\03\04\05\06\07"

; Native TSan sees that table[i] reads constant data and omits it, but not
; through the second GEP of table[i + 1] and table[i + 2]. Without table[i],
; those two reads merge into one 2-byte range instead of falling back to one
; native read each.
define i8 @constant_tail(i64 %n) sanitize_thread {
; CHECK-LABEL: define {{.*}} @constant_tail(
; CHECK-NOT:   call void @__tsan_read1(
; CHECK:       call void @__tsan_unaligned_read2(
; CHECK-NOT:   call void @__tsan_read1(
; CHECK:       ret i8
entry:
  %i = and i64 %n, 5
  %p0 = getelementptr inbounds i8, ptr @table, i64 %i
  %a = load i8, ptr %p0, align 1
  %p1 = getelementptr inbounds i8, ptr %p0, i64 1
  %b = load i8, ptr %p1, align 1
  %p2 = getelementptr inbounds i8, ptr %p0, i64 2
  %c = load i8, ptr %p2, align 1
  %ab = add i8 %a, %b
  %abc = add i8 %ab, %c
  ret i8 %abc
}

; A local whose address is not captured cannot race. Neither field access
; becomes an observation.
define i32 @local_pair(i64 %n) sanitize_thread {
; CHECK-LABEL: define {{.*}} @local_pair(
; CHECK-NOT:   call void @__tsan_
; CHECK:       ret i32
entry:
  %pair = alloca [4 x i32], align 4
  %i = and i64 %n, 3
  %p = getelementptr inbounds i32, ptr %pair, i64 %i
  store i32 1, ptr %p, align 4
  %q = getelementptr inbounds i32, ptr %p, i64 1
  store i32 2, ptr %q, align 4
  %r = getelementptr inbounds i32, ptr %pair, i64 %n
  %v = load i32, ptr %r, align 4
  ret i32 %v
}

; STATS:     MAIR stats
; STATS-NOT: native
; STATS:     select{{ +}}in 1  lowered 1{{$}}
; STATS-NOT: select
