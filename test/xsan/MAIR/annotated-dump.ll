; RUN: %mair_dump --pure --annotated %s | FileCheck %s

; CHECK: mair graph function
; CHECK-NEXT:   obs bb0 {site=bb-entry,bb=%entry
; CHECK-NEXT:   obs o1 {site=memory-inst,bb=%entry,anchor=load
; CHECK-NEXT:   obs o2 {site=memory-inst,bb=%entry,anchor=store
; CHECK-NEXT: {{^}$}}
; CHECK: define void @f
; CHECK: ; obs bb0 {site=bb-entry,bb=%entry
; CHECK: ; obs o1 {site=memory-inst,bb=%entry,anchor=load
; CHECK: ; obs o2 {site=memory-inst,bb=%entry,anchor=store

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}
