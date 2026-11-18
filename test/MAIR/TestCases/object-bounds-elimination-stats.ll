; -mair-stats counts the observations object bounds elimination deletes, and
; whether a proof needed LazyValueInfo after ScalarEvolution. Without trusted
; objects the pass does nothing and reports nothing.
; RUN: mair-opt -trust-constant-globals -mair-stats -passes=lpoc,obe -disable-output %s 2>&1 | FileCheck %s
; RUN: mair-opt -mair-stats -passes=lpoc,obe -disable-output %s 2>&1 | FileCheck %s --check-prefix=UNTRUSTED

@T = internal constant [256 x i8] zeroinitializer

; zext i8 is below 256 in SCEV's own range.
define i8 @byte_index(i8 %c) {
  %i = zext i8 %c to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %v = load i8, ptr %p, align 1
  ret i8 %v
}

; The range [0, n) at the loop exit: only LVI bounds n there, through the
; combined condition that enters the loop.
define void @bounded_range(i64 %n) {
entry:
  %small = icmp ule i64 %n, 256
  %nonzero = icmp ne i64 %n, 0
  %enter = and i1 %small, %nonzero
  br i1 %enter, label %loop, label %done
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %v = load volatile i8, ptr %p, align 1
  %next = add nuw nsw i64 %i, 1
  %more = icmp ult i64 %next, %n
  br i1 %more, label %loop, label %exit
exit:
  br label %done
done:
  ret void
}

; CHECK: MAIR stats
; CHECK: obe{{ +}}in 2  eliminated 2  scev 1  lvi 1{{$}}

; UNTRUSTED: MAIR stats
; UNTRUSTED-NOT: obe
