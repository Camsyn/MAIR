; RUN: mair-opt -O1 %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph,locationmerge %s | FileCheck %s

target datalayout = "e-p:64:64"

define void @wrapping_index(ptr %base, i8 %index) {
; CHECK-LABEL: mair graph of fn @wrapping_index {
; CHECK-DAG: o: read ptr %base + %index, 1
; CHECK-DAG: o: read ptr %base + %next, 1
entry:
  %first = getelementptr inbounds i8, ptr %base, i8 %index
  %next = add i8 %index, 1
  %second = getelementptr inbounds i8, ptr %base, i8 %next
  %first.value = load i8, ptr %first
  %second.value = load i8, ptr %second
  ret void
}

define void @nonwrapping_index(ptr %base, i8 %index) {
; CHECK-LABEL: mair graph of fn @nonwrapping_index {
; CHECK: o: read ptr %base + %index, 2
; CHECK-NOT: o:
entry:
  %first = getelementptr i8, ptr %base, i8 %index
  %next = add nsw i8 %index, 1
  %second = getelementptr i8, ptr %base, i8 %next
  %first.value = load i8, ptr %first
  %second.value = load i8, ptr %second
  ret void
}

define void @wrapping_chain(ptr %base, i8 %index) {
; CHECK-LABEL: mair graph of fn @wrapping_chain {
; CHECK-DAG: o: read ptr %base + %index, 2
; CHECK-DAG: o: read ptr %base + %second, 2
entry:
  %first = getelementptr i8, ptr %base, i8 %index
  %next = add i8 %index, 1
  %second.base = getelementptr i8, ptr %base, i8 %next
  %second = getelementptr i8, ptr %second.base, i64 1
  %first.value = load i16, ptr %first, align 1
  %second.value = load i16, ptr %second, align 1
  ret void
}
