; RUN: mair-opt --disable-output -passes="print<stat>" %s 2>&1 | FileCheck %s --check-prefix=STAT
; RUN: mair-opt -passes="print<stat>" %s > /dev/null 2>&1
; RUN: mair-opt -passes="print<stat>" -emit-llvm %s > /dev/null 2>&1
; RUN: not mair-opt -passes="unknown" %s 2>&1 | FileCheck %s --check-prefix=UNKNOWN

; STAT: MAIRStat for function: f
; STAT-NEXT:   program-points: 4
; STAT-NEXT:   observations: 2
; STAT: MAIRStat for function: no_access
; STAT-NEXT:   program-points: 2
; STAT-NEXT:   observations: 0

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}

define void @no_access(i32 %x) {
; UNKNOWN: unknown MAIRGraph pass 'unknown'
entry:
  %y = add i32 %x, 1
  ret void
}
