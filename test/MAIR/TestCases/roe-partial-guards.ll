; RUN: mair-opt -passes='roe<partial>' %s | FileCheck %s --implicit-check-not='o:'
; RUN: mair-opt -passes='simplifygraph,locationmerge,roe<partial>' %s | FileCheck %s --check-prefix=MERGED

; The join read postdominates the conditional read but has not run yet.
define void @postdom_only(ptr %p, i1 %take) {
; CHECK-LABEL: mair graph of fn @postdom_only {
; CHECK:         o: read ptr %p, 16, align 8
; CHECK:         o: read ptr %p + 8, 8, align 8
entry:
  br i1 %take, label %read, label %join
read:
  %target = load i128, ptr %p, align 8
  br label %join
join:
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %later = load i64, ptr %p8, align 8
  ret void
}

; The witness appears first in layout but one incoming edge bypasses it.
; Overlapping 16-byte reads avoid full ROE in the opposite direction.
define void @non_dominating_arm(ptr %p, i1 %take) {
; CHECK-LABEL: mair graph of fn @non_dominating_arm {
; CHECK:         o: read ptr %p, 16, align 8
; CHECK:         o: read ptr %p + 8, 16, align 8
entry:
  br i1 %take, label %arm, label %join
arm:
  %witness = load i128, ptr %p, align 8
  br label %join
join:
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %target = load i128, ptr %p8, align 8
  ret void
}

define void @unaligned_witness(ptr %p) {
; CHECK-LABEL: mair graph of fn @unaligned_witness {
; CHECK:         o: read ptr %p, 16, align 4
; CHECK:         o: read ptr %p + 8, 16, align 8
  %a = load i128, ptr %p, align 4
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = load i128, ptr %p8, align 8
  ret void
}

define void @unaligned_target(ptr %p) {
; CHECK-LABEL: mair graph of fn @unaligned_target {
; CHECK:         o: read ptr %p, 16, align 8
; CHECK:         o: read ptr %p + 8, 16, align 4
  %a = load i128, ptr %p, align 8
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = load i128, ptr %p8, align 4
  ret void
}

define void @volatile_witness(ptr %p) {
; CHECK-LABEL: mair graph of fn @volatile_witness {
; CHECK:         o: read volatile ptr %p, 16, align 8
; CHECK:         o: read ptr %p + 8, 16, align 8
  %a = load volatile i128, ptr %p, align 8
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = load i128, ptr %p8, align 8
  ret void
}

define void @volatile_target(ptr %p) {
; CHECK-LABEL: mair graph of fn @volatile_target {
; CHECK:         o: read ptr %p, 16, align 8
; CHECK:         o: read volatile ptr %p + 8, 16, align 8
  %a = load i128, ptr %p, align 8
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = load volatile i128, ptr %p8, align 8
  ret void
}

define void @write_witness(ptr %p) {
; CHECK-LABEL: mair graph of fn @write_witness {
; CHECK:         o: write ptr %p, 16, align 8
; CHECK:         o: read ptr %p + 8, 16, align 8
  store i128 0, ptr %p, align 8
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = load i128, ptr %p8, align 8
  ret void
}

define void @write_target(ptr %p) {
; CHECK-LABEL: mair graph of fn @write_target {
; CHECK:         o: read ptr %p, 16, align 8
; CHECK:         o: write ptr %p + 8, 16, align 8
  %a = load i128, ptr %p, align 8
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  store i128 0, ptr %p8, align 8
  ret void
}

define void @atomic_witness(ptr %p) {
; CHECK-LABEL: mair graph of fn @atomic_witness {
; CHECK:         o: read atomic ptr %p, 16, align 16
; CHECK:         o: read ptr %p + 8, 16, align 8
  %a = load atomic i128, ptr %p monotonic, align 16
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = load i128, ptr %p8, align 8
  ret void
}

define void @masked_target(ptr %p) {
; CHECK-LABEL: mair graph of fn @masked_target {
; CHECK:         o: read ptr %p, 16, align 8
; CHECK:         o: read masked ptr %p + 8, 16, mask <1,0>, align 8
  %a = load i128, ptr %p, align 8
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %b = call <2 x i64> @llvm.masked.load.v2i64.p0(ptr align 8 %p8, <2 x i1> <i1 true, i1 false>, <2 x i64> poison)
  ret void
}

declare <2 x i64> @llvm.masked.load.v2i64.p0(ptr, <2 x i1>, <2 x i64>)

; Two 4-byte reads are not one original 8-byte read, even after a merge
; gives their observation precisely the width and alignment of a witness.
define void @merged_witness(ptr %p, i1 %take) {
; CHECK-LABEL: mair graph of fn @merged_witness {
; CHECK:         o: read ptr %p, 4, align 8
; CHECK:         o: read ptr %p + 4, 4, align 4
; CHECK:         o: read ptr %p, 16, align 8
; MERGED-LABEL: mair graph of fn @merged_witness {
; MERGED:         o: read ptr %p, 8, align 8
; MERGED:         o: read ptr %p, 16, align 8
; MERGED-NOT:     o:
; MERGED:       }
entry:
  %a = load i32, ptr %p, align 8
  %p4 = getelementptr inbounds i8, ptr %p, i64 4
  %b = load i32, ptr %p4, align 4
  br i1 %take, label %read, label %exit
read:
  %target = load i128, ptr %p, align 8
  br label %exit
exit:
  ret void
}
