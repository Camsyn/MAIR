; RUN: mair-opt %s | FileCheck %s
; RUN: mair-opt -passes='default<O1>' %s | FileCheck %s

declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memcpy.element.unordered.atomic.p0.p0.i64(ptr, ptr, i64, i32 immarg)
declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, <4 x i1>, <4 x i32>)
declare void @llvm.masked.store.v4i32.p0(<4 x i32>, ptr, <4 x i1>)

define void @properties(ptr %p, ptr %q, i64 %size, <4 x i1> %mask,
                        <4 x i32> %values) {
entry:
  %volatile_load = load volatile i32, ptr %p, align 4
  %atomic_load = load atomic i32, ptr %p monotonic, align 4
  %rmw = atomicrmw add ptr %p, i32 1 monotonic
  %cas = cmpxchg ptr %p, i32 0, i32 1 monotonic monotonic
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %size, i1 false)
  call void @llvm.memcpy.p0.p0.i64(ptr %p, ptr %q, i64 %size, i1 true)
  call void @llvm.memmove.p0.p0.i64(ptr %q, ptr %p, i64 %size, i1 false)
  call void @llvm.memcpy.element.unordered.atomic.p0.p0.i64(
      ptr %p, ptr %q, i64 %size, i32 4)
  %masked_load = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> %mask, <4 x i32> zeroinitializer)
  call void @llvm.masked.store.v4i32.p0(
      <4 x i32> %values, ptr %q, <4 x i1> %mask)
  ret void
}

; CHECK-LABEL: mair graph of fn @properties {
; CHECK: ma1:
; CHECK-NEXT:   read volatile ptr %p, 4
; CHECK: ma2:
; CHECK-NEXT:   read atomic ptr %p, 4
; CHECK: ma3:
; CHECK-NEXT:   read/write atomic ptr %p, 4
; CHECK: ma4:
; CHECK-NEXT:   read/write atomic ptr %p, 4
; CHECK: ma5:
; CHECK-NEXT:   write call ptr %p, %size
; CHECK: ma6:
; CHECK-NEXT:   write volatile call ptr %p, %size
; CHECK-NEXT:   read volatile call ptr %q, %size
; CHECK: ma7:
; CHECK-DAG:    read call ptr %p, %size
; CHECK-DAG:    write call ptr %q, %size
; CHECK: ma8:
; CHECK-NEXT:   write atomic call ptr %p, %size
; CHECK-NEXT:   read atomic call ptr %q, %size
; CHECK: read masked ptr %p, 16
; CHECK: write masked ptr %q, 16
