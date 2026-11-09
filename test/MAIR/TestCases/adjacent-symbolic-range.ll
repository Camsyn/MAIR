; RUN: mair-opt -passes=lpoc %s | FileCheck %s --check-prefix=RANGES
; RUN: mair-opt -passes=lpoc,locationmerge %s | FileCheck %s --check-prefix=MERGED
; RUN: mair-opt -passes=lpoc,locationmerge --disable-output %s

define void @adjacent_bytes(ptr %p, i64 %n) {
; RANGES-LABEL: mair graph of fn @adjacent_bytes {
; RANGES:       pp9: loop-exit=%header,
; RANGES-NEXT:    o: read ptr %p, (1 + zext((18446744073709551615 + %n) to i65))
; RANGES-NEXT:    o: read ptr %p + 1, (1 + zext((18446744073709551615 + %n) to i65))
; RANGES-LABEL: mair graph of fn @adjacent_bytes of loop %header {
; RANGES:       pp3: preds=
; RANGES-EMPTY:
; MERGED-LABEL: mair graph of fn @adjacent_bytes {
; MERGED:       pp9: loop-exit=%header,
; MERGED-NEXT:    o: read ptr %p, ((1 + zext((18446744073709551615 + %n) to i65)) + 1)
; MERGED-NOT:     o: read ptr %p + 1
; MERGED-LABEL: mair graph of fn @adjacent_bytes of loop %header {
; MERGED:       pp3: preds=
; MERGED-EMPTY:
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %a = getelementptr i8, ptr %p, i64 %i
  %x = load i8, ptr %a, align 1
  %bidx = add i64 %i, 1
  %b = getelementptr i8, ptr %p, i64 %bidx
  %y = load i8, ptr %b, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %header

exit:
  ret void
}
