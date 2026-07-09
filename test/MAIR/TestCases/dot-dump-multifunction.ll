; RUN: not mair-opt -emit-dot %s 2>&1 | FileCheck %s

; CHECK: -emit-dot expects exactly one function definition; found 2

define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}

define void @g(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
