; RUN: mair-opt -emit-llvm -passes='roe<partial>' %s | FileCheck %s --check-prefix=PIN --implicit-check-not='o:'
; RUN: mair-opt -emit-llvm -passes='roe<partial>,simplifygraph,locationmerge,liom,lpoc,com,roe<partial>,roe,simplifygraph' %s | FileCheck %s --check-prefix=PIN --implicit-check-not='o:'
; RUN: mair-opt -passes='simplifygraph,locationmerge,liom' %s | FileCheck %s --check-prefix=CONTROL

; A witnesses B. B's old 16-byte source is not a witness for C, and C
; postdominates B and covers its new residual. A second ROE must not delete B.
define void @straight_chain(ptr %p) {
; PIN-LABEL: define void @straight_chain(
; PIN:         o: read ptr %p, 16, align 8
; PIN-NEXT:    %a = load i128, ptr %p, align 8
; PIN:         o: read ptr %p + 16, 8, align 8
; PIN-NEXT:    %b = load i128, ptr %p8, align 8
; PIN:         o: read ptr %p + 16, 16, align 8
; PIN-NEXT:    %c = load i128, ptr %p16, align 8
; CONTROL-LABEL: mair graph of fn @straight_chain {
; CONTROL:         o: read ptr %p, 32, align 8
entry:
  %a = load i128, ptr %p, align 8
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = load i128, ptr %p8, align 8
  %p16 = getelementptr inbounds i8, ptr %p, i64 16
  %c = load i128, ptr %p16, align 8
  ret void
}

; Both addresses are loop-invariant. Without the dependency the control
; pipeline merges the loads and moves the result out of the loop.
define void @loop_pins_both_ends(ptr %p, i1 %stop) {
; PIN-LABEL: define void @loop_pins_both_ends(
; PIN:       body:
; PIN:         o: read ptr %p + 8, 16, align 8
; PIN-NEXT:    %witness = load i128, ptr %p8, align 8
; PIN:         o: read ptr %p, 8, align 8
; PIN-NEXT:    %target = load i128, ptr %p, align 8
; CONTROL-LABEL: mair graph of fn @loop_pins_both_ends {
; CONTROL:         o: read ptr %p, 24, align 8
; CONTROL-LABEL: mair graph of fn @loop_pins_both_ends of loop %body {
; CONTROL-NOT:   o:
; CONTROL:       }
entry:
  br label %body
body:
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %witness = load i128, ptr %p8, align 8
  %target = load i128, ptr %p, align 8
  br i1 %stop, label %exit, label %body
exit:
  ret void
}
