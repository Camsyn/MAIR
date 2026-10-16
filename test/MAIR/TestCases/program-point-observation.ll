; RUN: mair-opt %s | FileCheck %s --check-prefix=MAIR
; RUN: mair-opt --disable-output -passes="print<stat>" %s 2>&1 | FileCheck %s --check-prefix=STAT

; A memcpy contributes two observations to one program point. Empty entry and
; exit program points remain separate containers.
; MAIR: mair graph of fn @copy {
; MAIR: pp0:
; MAIR: pp1:
; MAIR-NEXT:   o: write call ptr %dst, 4
; MAIR-NEXT:   o: read call ptr %src, 4
; MAIR: pp2:
; STAT: MAIRStat for function: copy
; STAT-NEXT:   program-points: 3
; STAT-NEXT:   observations: 2

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)

define void @copy(ptr %dst, ptr %src) {
entry:
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 4, i1 false)
  ret void
}
