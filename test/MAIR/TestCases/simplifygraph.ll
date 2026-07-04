; RUN: mair-opt -passes=simplifygraph %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph --disable-output %s

define void @f(i32* %p) {
; CHECK-LABEL: mair graph of fn @f {
; CHECK-NEXT:  bs0: preds=[], succs=[]
; CHECK-NEXT:    read ptr %p, 4
; CHECK-NEXT:    write ptr %p, 4
; CHECK-EMPTY:
; CHECK-NEXT:  }
; CHECK-NOT:   ma1:
; CHECK-NOT:   ma2:
; CHECK-NOT:   be3:
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}
