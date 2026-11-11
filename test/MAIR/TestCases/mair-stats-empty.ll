; RUN: mair-opt -mair-stats -passes='default<O1>' -disable-output %s 2>&1 | FileCheck %s
; RUN: mair-opt -passes='default<O1>' -disable-output %s 2>&1 | FileCheck %s --check-prefix=OFF --allow-empty

declare void @declaration()

; CHECK: MAIR stats
; CHECK-NEXT:   none
; CHECK-NOT: {{.}}
; OFF-NOT: {{.}}
