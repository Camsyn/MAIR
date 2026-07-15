; RUN: mair-opt -passes=simplifygraph %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph --disable-output %s
; RUN: mair-opt -passes=simplifygraph -emit-llvm %s | FileCheck %s --check-prefix=LLVM

define void @f(i32* %p) {
; CHECK-LABEL: mair graph of fn @f {
; CHECK-NEXT:  bs0: preds=[ma2(pdom)], succs=[ma2(dom)]
; CHECK-EMPTY:
; CHECK-NEXT:  ma2: preds=[be3(pdom), bs0(dom)], succs=[be3(dom), bs0(pdom)]
; CHECK-NEXT:    read ptr %p, 4
; CHECK-NEXT:    write ptr %p, 4
; CHECK-EMPTY:
; CHECK-NEXT:  be3: preds=[ma2(dom)], succs=[ma2(pdom)]
; CHECK-EMPTY:
; CHECK-NEXT:  }
; CHECK-NOT:   ma1:
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}

define void @sink_symbolic_pointer(ptr %p, i64 %i) {
; CHECK-LABEL: mair graph of fn @sink_symbolic_pointer {
; CHECK:       ma2: preds=
; CHECK-NEXT:    read ptr %p, 1
; CHECK-NEXT:    read ptr %p + %pi, 1
; LLVM-LABEL: define void @sink_symbolic_pointer(
; LLVM:         %first = load i8, ptr %p, align 1
; LLVM-NEXT:    %pi = getelementptr i8, ptr %p, i64 %i
; LLVM-NEXT:    ; ma2: preds=
; LLVM-NEXT:    ;   read ptr %p, 1
; LLVM-NEXT:    ;   read ptr %p + %pi, 1
; LLVM-NEXT:    %second = load i8, ptr %pi, align 1
entry:
  %first = load i8, ptr %p, align 1
  %pi = getelementptr i8, ptr %p, i64 %i
  %second = load i8, ptr %pi, align 1
  ret void
}
