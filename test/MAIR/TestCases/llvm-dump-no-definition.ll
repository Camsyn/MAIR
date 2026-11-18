; RUN: mair-opt -emit-llvm %s | FileCheck %s
; RUN: mair-opt -emit-llvm -disable-output %s | wc -c | FileCheck %s --check-prefix=EMPTY --match-full-lines

@counter = global i32 7
declare void @tick()

; CHECK: @counter = global i32 7
; CHECK: declare void @tick()
; EMPTY: 0
