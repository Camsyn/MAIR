; RUN: mair-opt -passes=roe %s | FileCheck %s
; RUN: mair-opt -passes=simplifygraph,locationmerge %s | FileCheck %s
; RUN: mair-opt -O1 %s | FileCheck %s

define void @narrow_first(ptr %p) {
; CHECK-LABEL: mair graph of fn @narrow_first {
; CHECK-COUNT-2: o: read ptr %p, unknown
  %narrow = load <vscale x 2 x i32>, ptr %p, align 4
  %wide = load <vscale x 4 x i32>, ptr %p, align 4
  ret void
}

define void @wide_first(ptr %p) {
; CHECK-LABEL: mair graph of fn @wide_first {
; CHECK-COUNT-2: o: read ptr %p, unknown
  %wide = load <vscale x 4 x i32>, ptr %p, align 4
  %narrow = load <vscale x 2 x i32>, ptr %p, align 4
  ret void
}

declare <vscale x 8 x i1> @llvm.masked.load.nxv8i1.p0(ptr, <vscale x 8 x i1>, <vscale x 8 x i1>)
declare <vscale x 8 x i8> @llvm.masked.load.nxv8i8.p0(ptr, <vscale x 8 x i1>, <vscale x 8 x i8>)

define void @same_mask(ptr %p, <vscale x 8 x i1> %mask) {
; CHECK-LABEL: mair graph of fn @same_mask {
; CHECK-COUNT-2: o: read masked ptr %p, unknown, mask %mask
  %bits = call <vscale x 8 x i1> @llvm.masked.load.nxv8i1.p0(ptr %p, <vscale x 8 x i1> %mask, <vscale x 8 x i1> poison)
  %bytes = call <vscale x 8 x i8> @llvm.masked.load.nxv8i8.p0(ptr %p, <vscale x 8 x i1> %mask, <vscale x 8 x i8> poison)
  ret void
}
