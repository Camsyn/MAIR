; RUN: mair-opt -emit-dot %s | FileCheck %s --check-prefix=DOT
; RUN: mair-opt -emit-dot -o %t.dot %s > %t.dot.stdout && FileCheck %s --check-prefix=DOT --input-file=%t.dot && wc -c < %t.dot.stdout | FileCheck %s --check-prefix=EMPTY
; RUN: not mair-opt -emit-dot -emit-llvm %s 2>&1 | FileCheck %s --check-prefix=CONFLICT

; EMPTY: 0
; DOT: digraph "MAIRGraph for f" {
; DOT: label="MAIRGraph for f";
; DOT-DAG: [label="dom", color=blue];
; DOT-DAG: [label="pdom", color=red];
; DOT-DAG: bs0
; DOT-DAG: bb.entry in entry
; DOT-DAG: ma1
; DOT-DAG: read ptr %p, 4
; DOT-DAG: ma2
; DOT-DAG: write ptr %p, 4
; DOT-DAG: be3
; CONFLICT: -emit-llvm and -emit-dot cannot be used together

define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  store i32 %v, ptr %p, align 4
  ret void
}
