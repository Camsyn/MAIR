; RUN: mair-opt -mair-stats -passes='liom,lpoc' -disable-output %s 2>&1 \
; RUN:   | FileCheck %s

; -mair-stats tells why the loop passes leave an observation in its loop.
; liom counts loop-invariant observations, lpoc the ones whose address varies.
; mair-opt has no barriers; the sanitizer tests cover rej-barrier.

declare void @use(i64)

; lpoc rej-nonaffine: the address is loaded, not a recurrence.
define void @indirect(ptr %a, ptr %idx) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %loop ]
  %pi = getelementptr inbounds i64, ptr %idx, i64 %i
  %k = load i64, ptr %pi, align 8
  %pa = getelementptr inbounds i8, ptr %a, i64 %k
  %v = load i8, ptr %pa, align 1
  %inc = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %inc, 16
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; lpoc rej-cond for the store: it runs only in some iterations. liom sinks
; the invariant load to the exit under a "has run" guard. The b load is
; summarized.
define void @conditional(ptr %a, ptr %b, ptr %c) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %latch ]
  %pb = getelementptr inbounds i64, ptr %b, i64 %i
  %v = load i64, ptr %pb, align 8
  %odd = trunc i64 %v to i1
  br i1 %odd, label %then, label %latch

then:
  %pa = getelementptr inbounds i64, ptr %a, i64 %i
  store i64 0, ptr %pa, align 8
  %w = load i64, ptr %c, align 8
  call void @use(i64 %w)
  br label %latch

latch:
  %inc = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %inc, 16
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; lpoc rej-multiexit for a[i], liom rej-multiexit for *c: the loop leaves from
; two blocks, and *c runs after the first exit test.
define i64 @find(ptr %a, ptr %c, i64 %n, i64 %x) {
entry:
  br label %loop

loop:
  %i = phi i64 [ %inc, %latch ], [ 0, %entry ]
  %pa = getelementptr inbounds i64, ptr %a, i64 %i
  %v = load i64, ptr %pa, align 8
  %hit = icmp eq i64 %v, %x
  br i1 %hit, label %found, label %latch

latch:
  %w = load i64, ptr %c, align 8
  call void @use(i64 %w)
  %inc = add nuw nsw i64 %i, 1
  %end = icmp eq i64 %inc, %n
  br i1 %end, label %miss, label %loop

found:
  ret i64 %i

miss:
  ret i64 -1
}

; lpoc rej-nocount: the loop leaves on loaded data, and its counter has no
; wrap flags while the accesses are more than a page apart, so no induction
; variable can stand in for a trip count.
define void @no_count(ptr %a) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %loop ]
  %off = mul i64 %i, 8192
  %pa = getelementptr i8, ptr %a, i64 %off
  %v = load i8, ptr %pa, align 1
  %more = icmp sgt i8 %v, -1
  %inc = add i64 %i, 1
  br i1 %more, label %loop, label %exit

exit:
  ret void
}

; The guarded load that liom sank out of the inner loop sits in the outer
; loop body; liom does not move guarded observations again: rej-cond.
define void @nested_conditional(ptr %b, ptr %c) {
entry:
  br label %outer

outer:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  br label %inner

inner:
  %j = phi i64 [ 0, %outer ], [ %j.next, %inner.latch ]
  %pb = getelementptr inbounds i64, ptr %b, i64 %j
  %v = load i64, ptr %pb, align 8
  %odd = trunc i64 %v to i1
  br i1 %odd, label %then, label %inner.latch

then:
  %w = load i64, ptr %c, align 8
  call void @use(i64 %w)
  br label %inner.latch

inner.latch:
  %j.next = add nuw nsw i64 %j, 1
  %inner.done = icmp eq i64 %j.next, 8
  br i1 %inner.done, label %outer.latch, label %inner

outer.latch:
  %i.next = add nuw nsw i64 %i, 1
  %outer.done = icmp eq i64 %i.next, 8
  br i1 %outer.done, label %exit, label %outer

exit:
  ret void
}

; CHECK:      liom{{ +}}in 12  exit 2  rej-cond 1  rej-multiexit 1{{$}}
; CHECK-NEXT: lpoc{{ +}}in 13  range 3  rej-nonaffine 1  rej-cond 1  rej-multiexit 1  rej-nocount 1{{$}}
