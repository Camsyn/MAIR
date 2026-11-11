; RUN: mair-opt -mair-stats -passes='default<O1>' -disable-output %s 2>&1 | FileCheck %s --check-prefix=O1
; RUN: mair-opt -mair-stats -passes='default<Og>' -disable-output %s 2>&1 | FileCheck %s --check-prefix=OG
; RUN: mair-opt -passes='default<O1>' -disable-output %s 2>&1 | FileCheck %s --check-prefix=OFF --allow-empty

; The inner loop synthesizes one 128-byte read. The second invariant-motion
; visit must hoist that read. LLVM -stats cannot show the two visits apart.

define i32 @repeated_row32(ptr %p, i32 %n) {
entry:
  %enter = icmp sgt i32 %n, 0
  br i1 %enter, label %outer.header, label %exit

outer.header:
  %i = phi i32 [ 0, %entry ], [ %inext, %outer.latch ]
  %s.outer = phi i32 [ 0, %entry ], [ %s.exit, %outer.latch ]
  br label %inner.header

inner.header:
  %j = phi i32 [ 0, %outer.header ], [ %jnext, %inner.header ]
  %s.inner = phi i32 [ %s.outer, %outer.header ], [ %s.next, %inner.header ]
  %j64 = sext i32 %j to i64
  %addr = getelementptr inbounds i32, ptr %p, i64 %j64
  %v = load i32, ptr %addr, align 4
  %s.next = add i32 %s.inner, %v
  %jnext = add i32 %j, 1
  %inner.done = icmp eq i32 %jnext, 32
  br i1 %inner.done, label %outer.latch, label %inner.header

outer.latch:
  %s.exit = phi i32 [ %s.next, %inner.header ]
  %inext = add i32 %i, 1
  %outer.done = icmp eq i32 %inext, %n
  br i1 %outer.done, label %exit, label %outer.header

exit:
  %s.ret = phi i32 [ 0, %entry ], [ %s.exit, %outer.latch ]
  ret i32 %s.ret
}

; O1: MAIR stats
; O1: pipeline O1
; O1: total
; O1: lpoc{{.*}}range 1
; O1: liom#2{{.*}}preheader 1
; O1-NOT: repeated_row32

; OG: MAIR stats
; OG: pipeline Og
; OG: total
; OG: lpoc{{.*}}range 1
; OG: liom#2{{.*}}preheader 1
; OG-NOT: repeated_row32

; OFF-NOT: {{.}}
