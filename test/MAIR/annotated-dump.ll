; RUN: %mair_opt %s | FileCheck %s --check-prefix=MAIR
; RUN: %mair_opt -emit-llvm %s | FileCheck %s --check-prefix=LLVM

; MAIR: mair graph function
; MAIR: obs bs0 {site=bb-entry,bb=%entry
; MAIR: obs ma1 {site=memory-inst,bb=%entry,anchor=load
; MAIR: obs ma2 {site=memory-inst,bb=%entry,anchor=store
; MAIR: obs be3 {site=bb-exit,bb=%entry

; LLVM: define void @f
; LLVM: entry:
; LLVM: ; obs bs0 {site=bb-entry,bb=%entry
; LLVM: ; obs ma1 {site=memory-inst,bb=%entry,anchor=load
; LLVM:   %v = load i32, ptr %p, align 4
; LLVM: ; obs ma2 {site=memory-inst,bb=%entry,anchor=store
; LLVM:   store i32 %v, ptr %p, align 4
; LLVM: ; obs be3 {site=bb-exit,bb=%entry
; LLVM:   ret void

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}
