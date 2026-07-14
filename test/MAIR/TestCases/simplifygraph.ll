; RUN: mair-opt -passes=simplifygraph %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph --disable-output %s

define void @f(i32* %p) {
; CHECK-LABEL: mair graph of fn @f {
; CHECK-NEXT:  bs0: preds=[ma1(pdom)], succs=[ma1(dom)]
; CHECK-EMPTY:
; CHECK-NEXT:  ma1: preds=[bs0(dom), be3(pdom)], succs=[bs0(pdom), be3(dom)]
; CHECK-NEXT:    read ptr %p, 4
; CHECK-NEXT:    write ptr %p, 4
; CHECK-EMPTY:
; CHECK-NEXT:  be3: preds=[ma1(dom)], succs=[ma1(pdom)]
; CHECK-EMPTY:
; CHECK-NEXT:  }
; CHECK-NOT:   ma2:
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}
