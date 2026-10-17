; RUN: mair-opt -emit-llvm %s | FileCheck %s
; RUN: mair-opt -passes=liom %s | FileCheck %s --check-prefix=MAIR

; Neither an LLVM preheader nor a dedicated exit exists in this input.
; Canonicalization creates real blocks before the hierarchy is constructed.
define i32 @normalize_loop(ptr %p, i1 %bypass, i1 %path, i32 %n) {
; CHECK-LABEL: define i32 @normalize_loop(
; CHECK:       header.preheader:
; CHECK:         ; pp{{[0-9]+}}: loop-preheader=%header,
; CHECK-NEXT:    br label %header
; CHECK:       header:
; CHECK-NEXT:    %iteration = phi i32 [ %next, %latch ], [ 0, %header.preheader ]
; CHECK:         ; o: read ptr %p, 4
; CHECK-NEXT:    %value = load i32, ptr %p, align 4
; CHECK:       exit.loopexit:
; CHECK:         ; pp{{[0-9]+}}: loop-exit=%header,
; CHECK-NEXT:    br label %exit
; CHECK:       exit:
; MAIR-LABEL: mair graph of fn @normalize_loop {
; MAIR:       loop-preheader=%header,
; MAIR-NEXT:    o: read ptr %p, 4
; MAIR:       loop-exit=%header,
; MAIR-LABEL: mair graph of fn @normalize_loop of loop %header {
; MAIR-NOT:   o:
; MAIR:       }
entry:
  br i1 %bypass, label %exit, label %dispatch
dispatch:
  br i1 %path, label %left, label %right
left:
  br label %header
right:
  br label %header
header:
  %iteration = phi i32 [ 0, %left ], [ 0, %right ], [ %next, %latch ]
  %value = load i32, ptr %p, align 4
  %done = icmp eq i32 %iteration, %n
  br i1 %done, label %exit, label %latch
latch:
  %next = add i32 %iteration, 1
  br label %header
exit:
  %result = phi i32 [ 0, %entry ], [ %value, %header ]
  ret i32 %result
}
