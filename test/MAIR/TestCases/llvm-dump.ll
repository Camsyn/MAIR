; RUN: %mair_opt %s | FileCheck %s --check-prefix=MAIR
; RUN: %mair_opt -emit-llvm %s | FileCheck %s --check-prefix=LLVM

; MAIR: mair graph function
; MAIR: obs bs0 {site=bb-entry,bb=%entry
; MAIR: obs ma1 {site=memory-inst,bb=%entry,anchor=load
; MAIR: obs ma2 {site=memory-inst,bb=%entry,anchor=store
; MAIR: obs be3 {site=bb-exit,bb=%entry
; MAIR: mair graph function @branch_loop
; MAIR: obs {{.*}} {site=bb-entry,bb=%entry
; MAIR: obs {{.*}} {site=bb-entry,bb=%then
; MAIR: obs {{.*}} {site=memory-inst,bb=%then,anchor=load
; MAIR: obs {{.*}} {site=memory-inst,bb=%then,anchor=store
; MAIR: obs {{.*}} {site=bb-entry,bb=%loop
; MAIR: obs {{.*}} {site=memory-inst,bb=%loop,anchor=load
; MAIR: obs {{.*}} {site=memory-inst,bb=%loop,anchor=store
; MAIR: obs {{.*}} {site=bb-exit,bb=%loop

; LLVM: define void @f
; LLVM: entry:
; LLVM: ; obs bs0 {site=bb-entry,bb=%entry
; LLVM: ; obs ma1 {site=memory-inst,bb=%entry,anchor=load
; LLVM:   %v = load i32, ptr %p, align 4
; LLVM: ; obs ma2 {site=memory-inst,bb=%entry,anchor=store
; LLVM:   store i32 %v, ptr %p, align 4
; LLVM: ; obs be3 {site=bb-exit,bb=%entry
; LLVM:   ret void
; LLVM: define void @branch_loop
; LLVM: entry:
; LLVM: ; obs {{.*}} {site=bb-entry,bb=%entry
; LLVM:   br i1 %c, label %then, label %loop
; LLVM: then:
; LLVM: ; obs {{.*}} {site=bb-entry,bb=%then
; LLVM: ; obs {{.*}} {site=memory-inst,bb=%then,anchor=load
; LLVM:   %tv = load i32, ptr %p, align 4
; LLVM: ; obs {{.*}} {site=memory-inst,bb=%then,anchor=store
; LLVM:   store i32 %tv, ptr %q, align 4
; LLVM: loop:
; LLVM: ; obs {{.*}} {site=bb-entry,bb=%loop
; LLVM: ; obs {{.*}} {site=memory-inst,bb=%loop,anchor=load
; LLVM:   %lv = load i32, ptr %p, align 4
; LLVM: ; obs {{.*}} {site=memory-inst,bb=%loop,anchor=store
; LLVM:   store i32 %next, ptr %q, align 4
; LLVM: ; obs {{.*}} {site=bb-exit,bb=%loop

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}

define void @branch_loop(i32* %p, i32* %q, i1 %c, i32 %n) {
entry:
  br i1 %c, label %then, label %loop

then:
  %tv = load i32, i32* %p, align 4
  store i32 %tv, i32* %q, align 4
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %lv = load i32, i32* %p, align 4
  %next = add i32 %i, 1
  store i32 %next, i32* %q, align 4
  %cond = icmp slt i32 %next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}
