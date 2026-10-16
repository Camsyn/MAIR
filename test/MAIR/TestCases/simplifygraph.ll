; RUN: mair-opt -passes=simplifygraph %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph --disable-output %s
; RUN: mair-opt -passes=simplifygraph -emit-llvm %s | FileCheck %s --check-prefix=LLVM

define void @f(i32* %p) {
; CHECK-LABEL: mair graph of fn @f {
; CHECK-NEXT:  pp0: preds=[pp2(pdom)], succs=[pp2(dom)]
; CHECK-EMPTY:
; CHECK-NEXT:  pp2: preds=[pp3(pdom), pp0(dom)], succs=[pp3(dom), pp0(pdom)]
; CHECK-NEXT:    o: read ptr %p, 4
; CHECK-NEXT:    o: write ptr %p, 4
; CHECK-EMPTY:
; CHECK-NEXT:  pp3: preds=[pp2(dom)], succs=[pp2(pdom)]
; CHECK-EMPTY:
; CHECK-NEXT:  }
; CHECK-NOT:   pp1:
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}

define void @sink_symbolic_pointer(ptr %p, i64 %i) {
; CHECK-LABEL: mair graph of fn @sink_symbolic_pointer {
; CHECK:       pp2: preds=
; CHECK-NEXT:    o: read ptr %p, 1
; CHECK-NEXT:    o: read ptr %p + %pi, 1
; LLVM-LABEL: define void @sink_symbolic_pointer(
; LLVM:         %first = load i8, ptr %p, align 1
; LLVM-NEXT:    %pi = getelementptr i8, ptr %p, i64 %i
; LLVM-NEXT:    ; pp2: preds=
; LLVM-NEXT:    ;   o: read ptr %p, 1
; LLVM-NEXT:    ;   o: read ptr %p + %pi, 1
; LLVM-NEXT:    %second = load i8, ptr %pi, align 1
entry:
  %first = load i8, ptr %p, align 1
  %pi = getelementptr i8, ptr %p, i64 %i
  %second = load i8, ptr %pi, align 1
  ret void
}
