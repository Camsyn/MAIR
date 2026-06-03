; Verify the minimal UBSan CFG shape reduced from bfd_elf_size_dynamic_sections.
; The old failure left an archived AddRecord block as a live predecessor of
; cont2552 when cont2555 was the first block with user instructions.
; RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-dump=%t.default.dump -S %s -o %t.default.out.ll
; RUN: FileCheck %s --check-prefix=DEFAULT --input-file=%t.default.dump/02-desan.ll
; RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-de-ubsan -xsan-dump=%t.dump -S %s -o %t.out.ll
; RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/02-desan.ll
; RUN: FileCheck %s --check-prefix=FINAL --input-file=%t.dump/10-final.ll

target triple = "x86_64-unknown-linux-gnu"

; Test CFG structure
;      A
;     / \
;    /   |
;    B   |
;    | \ /
;    |  C
;    | /
;     D
;     | \
;     |  E
;     | /
;     F

@data = private global i32 0, align 4

define void @bfd_elf_size_dynamic_sections(i1 %cond0, i1 %cond1, ptr %p) sanitize_address sanitize_memory sanitize_thread {
entry:
  br label %entry.to.check

entry.to.check:
  br i1 %cond0, label %if.end2550, label %cont2537.thread

if.end2550:
  br i1 %cond0, label %cont2552, label %handler.type_mismatch2551, !prof !1, !nosanitize !0

cont2537.thread:
  br label %handler.type_mismatch2551

handler.type_mismatch2551:
  call void @__ubsan_handle_type_mismatch_v1(ptr @data, i64 0), !nosanitize !0
  br label %cont2552, !nosanitize !0

cont2552:
  br i1 %cond1, label %cont2555, label %handler.type_mismatch2554, !prof !1, !nosanitize !0

handler.type_mismatch2554:
  call void @__ubsan_handle_type_mismatch_v1(ptr @data, i64 1), !nosanitize !0
  br label %cont2555, !nosanitize !0

cont2555:
  %san.phi = phi i64 [ 2, %cont2552 ], [ 2, %handler.type_mismatch2554 ]
  call void @__ubsan_handle_type_mismatch_v1(ptr @data, i64 %san.phi), !nosanitize !0
  ret void
}

declare void @__ubsan_handle_type_mismatch_v1(ptr, i64)

!0 = !{}
!1 = !{!"branch_weights", i32 1048575, i32 1}

; DEFAULT-LABEL: define void @bfd_elf_size_dynamic_sections(
; DEFAULT: if.end2550:
; DEFAULT-NEXT: br i1 {{.*}}, label %cont2552, label %handler.type_mismatch2551
; DEFAULT: handler.type_mismatch2551:
; DEFAULT-NEXT: call void @__ubsan_handle_type_mismatch_v1
; DEFAULT-NEXT: br label %cont2552
; DEFAULT: cont2552:
; DEFAULT-NEXT: br i1 {{.*}}, label %cont2555, label %handler.type_mismatch2554
; DEFAULT: handler.type_mismatch2554:
; DEFAULT-NEXT: call void @__ubsan_handle_type_mismatch_v1
; DEFAULT-NEXT: br label %cont2555
; DEFAULT: cont2555:
; DEFAULT-NEXT: %san.phi = phi i64
; DEFAULT-NEXT: call void @__ubsan_handle_type_mismatch_v1
; DEFAULT: }

; DESAN-LABEL: define void @bfd_elf_size_dynamic_sections(
; DESAN-NOT: <badref>
; DESAN: if.end2550:
; DESAN-NEXT: br i1 {{.*}}, label %cont2552, label %handler.type_mismatch2551
; DESAN: cont2537.thread:
; DESAN-NEXT: br label %handler.type_mismatch2551
; DESAN: handler.type_mismatch2551:
; DESAN-NEXT: br label %cont2552
; DESAN: cont2552:{{[ ]+}}; preds = %handler.type_mismatch2551, %if.end2550
; DESAN-NOT: __xsan_desan_add_record_bb
; DESAN-NEXT: ret void
; DESAN: }

; FINAL-LABEL: define void @bfd_elf_size_dynamic_sections(
; FINAL: call void @__ubsan_handle_type_mismatch_v1{{.*}}!xsan.ubsan
; FINAL: ret void{{.*}}!xsan.user
