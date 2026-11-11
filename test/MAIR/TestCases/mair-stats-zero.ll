; RUN: mair-opt -mair-stats -passes='liom,lpoc' -disable-output %s 2>&1 | FileCheck %s

define void @empty() {
  ret void
}

; CHECK: MAIR stats
; CHECK-NEXT:   functions 1
; CHECK-NEXT:   total
; CHECK-NOT: {{.}}
