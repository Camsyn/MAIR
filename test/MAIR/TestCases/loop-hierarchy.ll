; RUN: mair-opt -passes=roe %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph,roe %s | FileCheck %s --check-prefix=SIMPLIFY
; RUN: mair-opt -passes=roe -emit-llvm %s | FileCheck %s --check-prefix=LLVM

; A pre-loop store cannot cover a load in a later iteration after free.
; mair-opt has no sanitizer-specific barrier trait: the hierarchy itself must
; prevent ordinary elimination from connecting the two scopes.
declare void @free(ptr)

define i32 @lifetime_after_free(ptr %p) {
; CHECK-LABEL: mair graph of fn @lifetime_after_free {
; CHECK:       o: write ptr %p, 4
; CHECK:       loop-preheader=%loop,
; CHECK:       loop-exit=%loop,
; CHECK:       }
; CHECK-LABEL: mair graph of fn @lifetime_after_free of loop %loop {
; CHECK:       o: read ptr %p, 4
; CHECK-NOT:   o:
; CHECK:       }
; SIMPLIFY-LABEL: mair graph of fn @lifetime_after_free {
; SIMPLIFY:       loop-preheader=%loop,
; SIMPLIFY:       loop-exit=%loop,
; SIMPLIFY-LABEL: mair graph of fn @lifetime_after_free of loop %loop {
; SIMPLIFY:       o: read ptr %p, 4
; LLVM-LABEL: define i32 @lifetime_after_free(
; LLVM:         ; o: write ptr %p, 4
; LLVM-NEXT:    store i32 1, ptr %p, align 4
; LLVM:       loop:
; LLVM:         ; o: read ptr %p, 4
; LLVM-NEXT:    %value = load i32, ptr %p, align 4
; LLVM:         call void @free(ptr %released)
entry:
  store i32 1, ptr %p, align 4
  br label %loop
loop:
  %iteration = phi i32 [ 0, %entry ], [ %next, %loop ]
  %value = load i32, ptr %p, align 4
  %first = icmp eq i32 %iteration, 0
  %released = select i1 %first, ptr %p, ptr null
  call void @free(ptr %released)
  %next = add i32 %iteration, 1
  %done = icmp eq i32 %next, 2
  br i1 %done, label %exit, label %loop
exit:
  ret i32 %value
}

define i32 @same_loop_redundancy(ptr %p, i1 %done) {
; CHECK-LABEL: mair graph of fn @same_loop_redundancy {
; CHECK-NOT:   o:
; CHECK:       loop-preheader=%loop,
; CHECK:       loop-exit=%loop,
; CHECK-LABEL: mair graph of fn @same_loop_redundancy of loop %loop {
; CHECK:       o: read ptr %p, 4
; CHECK-NOT:   o:
; CHECK:       }
entry:
  br label %loop
loop:
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %p, align 4
  %sum = add i32 %first, %second
  br i1 %done, label %exit, label %loop
exit:
  ret i32 %sum
}

define void @nested_scopes(ptr %p, i1 %inner.done, i1 %outer.done) {
; CHECK-LABEL: mair graph of fn @nested_scopes {
; CHECK:       o: read ptr %p, 4
; CHECK:       loop-preheader=%outer,
; CHECK:       loop-exit=%outer,
; CHECK-LABEL: mair graph of fn @nested_scopes of loop %outer {
; CHECK:       o: read ptr %p, 4
; CHECK:       loop-preheader=%inner,
; CHECK:       loop-exit=%inner,
; CHECK-LABEL: mair graph of fn @nested_scopes of loop %inner {
; CHECK:       o: read ptr %p, 4
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %before = load i32, ptr %p, align 4
  br label %outer
outer:
  %outer.value = load i32, ptr %p, align 4
  br label %inner
inner:
  %inner.value = load i32, ptr %p, align 4
  br i1 %inner.done, label %outer.latch, label %inner
outer.latch:
  br i1 %outer.done, label %exit, label %outer
exit:
  ret void
}

define void @multiple_exits(ptr %p, i1 %early, i1 %done) {
; CHECK-LABEL: mair graph of fn @multiple_exits {
; CHECK:       loop-preheader=%loop,
; CHECK:       loop-exit=%loop,
; CHECK:       loop-exit=%loop,
; CHECK-NOT:   loop-exit=
; CHECK:       }
; CHECK-LABEL: mair graph of fn @multiple_exits of loop %loop {
; CHECK:       o: read ptr %p, 4
entry:
  br label %loop
loop:
  %value = load i32, ptr %p, align 4
  br i1 %early, label %early.exit, label %latch
latch:
  br i1 %done, label %normal.exit, label %loop
early.exit:
  ret void
normal.exit:
  ret void
}

define void @no_exit(ptr %p) {
; CHECK-LABEL: mair graph of fn @no_exit {
; CHECK:       loop-preheader=%loop,
; CHECK-NOT:   loop-exit=
; CHECK:       }
; CHECK-LABEL: mair graph of fn @no_exit of loop %loop {
; CHECK:       o: read ptr %p, 4
; CHECK-NOT:   loop-preheader=
; CHECK-NOT:   loop-exit=
; CHECK:       }
entry:
  br label %loop
loop:
  %value = load i32, ptr %p, align 4
  br label %loop
}
