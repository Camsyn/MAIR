; RUN: mair-opt -mair-stats -passes='liom,lpoc' -disable-output %s 2>&1 | FileCheck %s --check-prefix=ONCE
; RUN: mair-opt -mair-stats -passes='liom,liom' -disable-output %s 2>&1 | FileCheck %s --check-prefix=TWICE
; RUN: mair-opt -mair-stats -passes='print<stat>' -disable-output %s 2>&1 | FileCheck %s --check-prefix=NO-VISITS

define void @empty() {
  ret void
}

define i32 @first(ptr %p) {
  %value = load i32, ptr %p
  ret i32 %value
}

define i32 @second(ptr %p) {
  %value = load i32, ptr %p
  ret i32 %value
}

; ONCE: MAIR stats
; ONCE-NEXT:   functions 3
; ONCE-NEXT:   total
; ONCE-NEXT:     liom{{ +}}in 2{{$}}
; ONCE-NEXT:     lpoc{{ +}}in 2{{$}}
; ONCE-NOT: {{.}}

; TWICE: MAIR stats
; TWICE-NEXT:   functions 3
; TWICE-NEXT:   total
; TWICE-NEXT:     liom{{ +}}in 4{{$}}
; TWICE-NOT: {{.}}

; NO-VISITS: MAIR stats
; NO-VISITS-NEXT:   none
; NO-VISITS-NOT: {{.}}
