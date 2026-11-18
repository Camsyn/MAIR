; RUN: env MAIR=1 %clang -asan -O3 -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=COMMON,PART
; RUN: env MAIR=1 %clang -asan -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=COMMON,FULL
; RUN: env MAIR=1 %clang -asan -O1 -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=COMMON,FULL
; RUN: env MAIR=1 %clang -asan -Og -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=COMMON,FULL
; RUN: env MAIR=1 %clang -asan -Os -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=COMMON,FULL
; RUN: env MAIR=1 %clang -asan -Oz -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=COMMON,FULL
; RUN: env MAIR=1 %clang -asan -O0 -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=COMMON,FULL
; RUN: env MAIR=1 %clang -asan -O3 -mllvm -as-instrumentation-with-call-threshold=0 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=CALLS --implicit-check-not='call void @__asan_load'
; RUN: env MAIR=1 %clang -asan -O3 -mllvm -as-instrumentation-with-call-threshold=1 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=CALLS --implicit-check-not='call void @__asan_load'
; RUN: env MAIR=1 %clang -asan -O3 -mllvm -as-instrumentation-with-call-threshold=1 -mllvm -as-optimize-callbacks -S -emit-llvm %s -o - | FileCheck %s --check-prefix=INTRINSIC
; RUN: env MAIR=1 %clang -asan -O3 -fsanitize=address,alignment -v -S -emit-llvm %s -o %t.mixed.ll 2> %t.cc1
; RUN: FileCheck %s --check-prefix=MIXED-CC1 --input-file=%t.cc1
; RUN: FileCheck %s --check-prefixes=COMMON,FULL --input-file=%t.mixed.ll

; Check the effective cc1 command, not just the wrapper arguments.
; MIXED-CC1: -cc1{{"? .*}}-fsanitize={{(address,alignment|alignment,address)}}{{"? }}

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; In either direction the fast path tests one shadow byte, but the failure
; still reports the base address, width and location of the original load.
define <2 x i64> @right(ptr %p) sanitize_address !dbg !5 {
; COMMON-LABEL: define {{.*}} @right(
; COMMON:         call void @__asan_report_load8(
; COMMON:         %witness = load i64, ptr %p
; COMMON:         br i1
; COMMON:         [[BASE:%[^ ]+]] = ptrtoint ptr %p to i64
; PART:           [[INDEX:%[^ ]+]] = lshr i64 [[BASE]], 3
; PART:           [[SHADOW:%[^ ]+]] = add i64 [[INDEX]], 2147450880
; PART:           [[HIGH:%[^ ]+]] = add i64 [[SHADOW]], 1
; PART:           [[SP:%[^ ]+]] = inttoptr i64 [[HIGH]] to ptr
; PART:           [[V:%[^ ]+]] = load i8, ptr [[SP]],
; PART:           [[BAD:%[^ ]+]] = icmp ne i8 [[V]], 0
; PART:           br i1 [[BAD]], label %[[FAIL:[^,]+]],
; PART:           [[FAIL]]:
; FULL:           load i16, ptr
; COMMON:         call void @__asan_report_load16(i64 [[BASE]]){{.*}}!dbg [[RIGHT:![0-9]+]]
; COMMON:         load <2 x i64>, ptr %p, align 8, !dbg [[RIGHT]]
; CALLS-LABEL: define {{.*}} @right(
; CALLS:         call void @__asan_load8(
; CALLS:         [[ORIG:%[^ ]+]] = ptrtoint ptr %p to i64
; CALLS:         call void @__asan_load16(i64 [[ORIG]])
; CALLS:         ret <2 x i64>
; INTRINSIC-LABEL: define {{.*}} @right(
; INTRINSIC:       call void @llvm.asan.check.memaccess(ptr %p, i32 6)
; INTRINSIC:       call void @llvm.asan.check.memaccess(ptr %p, i32 8), !dbg [[R:![0-9]+]]
; INTRINSIC:       load <2 x i64>, ptr %p, align 8, !dbg [[R]]
entry:
  %witness = load i64, ptr %p, align 8, !dbg !8
  %take = icmp ne i64 %witness, 0
  br i1 %take, label %read, label %exit
read:
  %wide = load <2 x i64>, ptr %p, align 8, !dbg !9
  br label %exit
exit:
  %result = phi <2 x i64> [ zeroinitializer, %entry ], [ %wide, %read ]
  ret <2 x i64> %result
}

define <2 x i64> @left(ptr %p) sanitize_address !dbg !10 {
; COMMON-LABEL: define {{.*}} @left(
; COMMON:         call void @__asan_report_load8(
; COMMON:         %witness = load i64,
; COMMON:         br i1
; COMMON:         [[BASE:%[^ ]+]] = ptrtoint ptr %p to i64
; PART:           [[INDEX:%[^ ]+]] = lshr i64 [[BASE]], 3
; PART:           [[SHADOW:%[^ ]+]] = add i64 [[INDEX]], 2147450880
; PART:           [[SP:%[^ ]+]] = inttoptr i64 [[SHADOW]] to ptr
; PART:           [[V:%[^ ]+]] = load i8, ptr [[SP]],
; PART:           [[BAD:%[^ ]+]] = icmp ne i8 [[V]], 0
; PART:           br i1 [[BAD]], label %[[FAIL:[^,]+]],
; PART:           [[FAIL]]:
; FULL:           load i16, ptr
; COMMON:         call void @__asan_report_load16(i64 [[BASE]]){{.*}}!dbg [[LEFT:![0-9]+]]
; COMMON:         load <2 x i64>, ptr %p, align 8, !dbg [[LEFT]]
; CALLS-LABEL: define {{.*}} @left(
; CALLS:         call void @__asan_load8(
; CALLS:         [[ORIG:%[^ ]+]] = ptrtoint ptr %p to i64
; CALLS:         call void @__asan_load16(i64 [[ORIG]])
; CALLS:         ret <2 x i64>
; INTRINSIC-LABEL: define {{.*}} @left(
; INTRINSIC:       call void @llvm.asan.check.memaccess({{.*}}, i32 6)
; INTRINSIC:       call void @llvm.asan.check.memaccess(ptr %p, i32 8), !dbg [[L:![0-9]+]]
; INTRINSIC:       load <2 x i64>, ptr %p, align 8, !dbg [[L]]
entry:
  %p8 = getelementptr inbounds i8, ptr %p, i64 8
  %witness = load i64, ptr %p8, align 8, !dbg !11
  %take = icmp ne i64 %witness, 0
  br i1 %take, label %read, label %exit
read:
  %wide = load <2 x i64>, ptr %p, align 8, !dbg !12
  br label %exit
exit:
  %result = phi <2 x i64> [ zeroinitializer, %entry ], [ %wide, %read ]
  ret <2 x i64> %result
}

; The unknown call can change shadow state. The first load must not be
; reused across it even though it still dominates the second in LLVM IR.
declare void @barrier()
define <2 x i64> @across_barrier(ptr %p) sanitize_address {
; COMMON-LABEL: define {{.*}} @across_barrier(
; COMMON:         call void @__asan_report_load8(
; COMMON:         call void @barrier()
; COMMON:         load i16, ptr
; COMMON:         call void @__asan_report_load16(
; COMMON:         load <2 x i64>, ptr %p, align 8
; CALLS-LABEL: define {{.*}} @across_barrier(
; CALLS:         call void @__asan_load8(
; CALLS:         call void @barrier()
; CALLS:         call void @__asan_load16(
; CALLS:         ret <2 x i64>
entry:
  %witness = load i64, ptr %p, align 8
  %take = icmp ne i64 %witness, 0
  br i1 %take, label %read, label %exit
read:
  call void @barrier()
  %wide = load <2 x i64>, ptr %p, align 8
  br label %exit
exit:
  %result = phi <2 x i64> [ zeroinitializer, %entry ], [ %wide, %read ]
  ret <2 x i64> %result
}

; COMMON-DAG: [[RIGHT]] = !DILocation(line: 21, column: 7,
; COMMON-DAG: [[LEFT]] = !DILocation(line: 41, column: 9,

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "mair-test", isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly)
!1 = !DIFile(filename: "partial.c", directory: "/")
!2 = !DISubroutineType(types: !6)
!3 = !{i32 2, !"Dwarf Version", i32 5}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = distinct !DISubprogram(name: "right", scope: !1, file: !1, line: 10, type: !2, scopeLine: 10, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!6 = !{}
!8 = !DILocation(line: 12, column: 5, scope: !5)
!9 = !DILocation(line: 21, column: 7, scope: !5)
!10 = distinct !DISubprogram(name: "left", scope: !1, file: !1, line: 30, type: !2, scopeLine: 30, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!11 = !DILocation(line: 32, column: 3, scope: !10)
!12 = !DILocation(line: 41, column: 9, scope: !10)
