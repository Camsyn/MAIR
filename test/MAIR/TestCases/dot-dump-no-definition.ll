; RUN: not mair-opt -emit-dot %s 2>&1 | FileCheck %s

; CHECK: -emit-dot expects exactly one function definition; found 0

declare void @f(ptr %p)
