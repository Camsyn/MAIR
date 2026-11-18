; RUN: mair-opt -passes='roe<partial>' %s | FileCheck %s --check-prefixes=BOTH,PART --implicit-check-not='o:'
; RUN: mair-opt -passes='roe<partial>,roe<partial>,roe' %s | FileCheck %s --check-prefixes=BOTH,PART --implicit-check-not='o:'
; RUN: mair-opt -O3 %s | FileCheck %s --check-prefixes=BOTH,PART --implicit-check-not='o:'
; RUN: mair-opt -passes=roe %s | FileCheck %s --check-prefixes=BOTH,FULL --implicit-check-not='o:'
; RUN: mair-opt -O1 %s | FileCheck %s --check-prefixes=BOTH,FULL --implicit-check-not='o:'

; The skip edge keeps the second read from justifying a check at entry.
define void @keep_right(ptr %p, i1 %take) {
; BOTH-LABEL: mair graph of fn @keep_right {
; BOTH:         o: read ptr %p, 16, align 8
; PART:         o: read ptr %p + 16, 8, align 8
; FULL:         o: read ptr %p + 8, 16, align 8
entry:
  %witness = load <2 x i64>, ptr %p, align 8
  br i1 %take, label %read, label %exit
read:
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %target = load i128, ptr %p8, align 8
  br label %exit
exit:
  ret void
}

define void @keep_left(ptr %p, i1 %take) {
; BOTH-LABEL: mair graph of fn @keep_left {
; BOTH:         o: read ptr %p + 8, 8, align 8
; PART:         o: read ptr %p, 8, align 8
; FULL:         o: read ptr %p, 16, align 8
entry:
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %witness = load i64, ptr %p8, align 8
  br i1 %take, label %read, label %exit
read:
  %target = load <2 x i64>, ptr %p, align 8
  br label %exit
exit:
  ret void
}

; A checks [0,8). B can consequently check just [8,16), but B is
; conditional. At the join, C still needs both halves of [8,24).
define void @bypass_middle_of_chain(ptr %p, i1 %take) {
; BOTH-LABEL: mair graph of fn @bypass_middle_of_chain {
; BOTH:         o: read ptr %p, 8, align 8
; PART:         o: read ptr %p + 8, 8, align 8
; FULL:         o: read ptr %p, 16, align 8
; BOTH:         o: read ptr %p + 8, 16, align 8
entry:
  %a = load i64, ptr %p, align 8
  br i1 %take, label %some, label %join
some:
  %b = load i128, ptr %p, align 8
  br label %join
join:
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %c = load i128, ptr %p8, align 8
  ret void
}
