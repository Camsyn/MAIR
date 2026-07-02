; RUN: %mair_dump --pure --annotated %s | FileCheck %s

; CHECK: mair graph function
; CHECK: obs bs0 {site=bb-entry,bb=%entry
; CHECK: obs ma1 {site=memory-inst,bb=%entry,anchor=load
; CHECK: obs ma2 {site=memory-inst,bb=%entry,anchor=store
; CHECK: obs be3 {site=bb-exit,bb=%entry
; CHECK: define void @f
; CHECK:   ; obs bs0 {site=bb-entry,bb=%entry
; CHECK: entry:
; CHECK:   ; obs ma1 {site=memory-inst,bb=%entry,anchor=load
; CHECK:   %v = load i32, i32* %p, align 4
; CHECK:   ; obs ma2 {site=memory-inst,bb=%entry,anchor=store
; CHECK:   store i32 %v, i32* %p, align 4
; CHECK:   ; obs be3 {site=bb-exit,bb=%entry
; CHECK:   ret void

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}
