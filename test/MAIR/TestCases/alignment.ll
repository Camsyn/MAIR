; Every region records the alignment known for its first byte: the alignment
; of its address, printed as `align N` when N > 1. An access proves the
; alignment it is declared with. A merged region starts where its leftmost
; part starts, which proves that part's alignment, and a part a known
; distance d past the start proves the alignment common to its own and d.
; The merge keeps the strongest. Sanitizers read it from the region (TSan to
; choose its aligned entries, ASan its inline checks).
; RUN: mair-opt -passes="simplifygraph,locationmerge" %s | FileCheck %s
; RUN: mair-opt -passes="lpoc,locationmerge" %s | FileCheck %s --check-prefix=LOOP

; The leftmost access proves 8.
define void @leftmost(ptr %p) {
; CHECK-LABEL: mair graph of fn @leftmost {
; CHECK:         o: read ptr %p, 8, align 8{{$}}
entry:
  %q = getelementptr inbounds i8, ptr %p, i64 4
  %a = load i32, ptr %p, align 8
  %b = load i32, ptr %q, align 4
  ret void
}

; An underaligned i64 starts the range, but the 16-aligned i64 eight bytes
; past it proves the start 8-aligned.
define void @later_part_proves_more(ptr %p) {
; CHECK-LABEL: mair graph of fn @later_part_proves_more {
; CHECK:         o: read ptr %p, 16, align 8{{$}}
entry:
  %q = getelementptr inbounds i8, ptr %p, i64 8
  %a = load i64, ptr %p, align 4
  %b = load i64, ptr %q, align 16
  ret void
}

; The start of the range is the second access's: 4 bytes past %p.
define void @start_past_base(ptr %p) {
; CHECK-LABEL: mair graph of fn @start_past_base {
; CHECK:         o: read ptr %p + 4, 8, align 4{{$}}
entry:
  %q = getelementptr inbounds i8, ptr %p, i64 4
  %r = getelementptr inbounds i8, ptr %p, i64 8
  %a = load i32, ptr %r, align 8
  %b = load i32, ptr %q, align 4
  ret void
}

; Nothing proves more than one byte.
define void @unknown(ptr %p) {
; CHECK-LABEL: mair graph of fn @unknown {
; CHECK:         o: read ptr %p, 1{{$}}
entry:
  %a = load i8, ptr %p, align 1
  ret void
}

; A masked access proves the alignment of its pointer.
define void @masked(ptr %p) {
; CHECK-LABEL: mair graph of fn @masked {
; CHECK:         o: read masked ptr %p, 16, mask <1,0,1,1>, align 16{{$}}
entry:
  %v = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 16 %p, <4 x i1> <i1 true, i1 false, i1 true, i1 true>, <4 x i32> poison)
  ret void
}

; Every trip of a loop access proves its declared alignment, so the range of
; all trips starts with it, downward as well as upward.
define void @range_up(ptr %p) {
; LOOP-LABEL: mair graph of fn @range_up {
; LOOP:         o: read ptr %p, 64, align 4{{$}}
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %a = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load i32, ptr %a, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, 16
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

define void @range_down(ptr %p) {
; LOOP-LABEL: mair graph of fn @range_down {
; LOOP:         o: read ptr %p, 64, align 8{{$}}
entry:
  br label %loop

loop:
  %i = phi i64 [ 15, %entry ], [ %next, %loop ]
  %a = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load i32, ptr %a, align 8
  %next = add nsw i64 %i, -1
  %done = icmp eq i64 %i, 0
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; s[i].a and s[i].b: every trip of the merged region starts at s[i].a.
define void @periodic_fields(ptr %s, i64 %n) {
; LOOP-LABEL: mair graph of fn @periodic_fields {
; LOOP:         o: read period start=%s, stride=12, access-size=8, trips={{.*}}, align 4{{$}}
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.12 = mul nuw nsw i64 %i, 12
  %a = getelementptr inbounds i8, ptr %s, i64 %i.12
  %b = getelementptr inbounds i8, ptr %a, i64 4
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Interleaved slices: a trip of the union is a trip of some slice, so it
; proves the weakest slice alignment, here the 8 of the +8 slice.
define void @interleaved(ptr %p, i64 %n) {
; LOOP-LABEL: mair graph of fn @interleaved {
; LOOP:         o: read period start=%p, stride=8, access-size=4, trips={{.*}}, align 8{{$}}
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.16 = mul nuw nsw i64 %i, 16
  %a = getelementptr inbounds i8, ptr %p, i64 %i.16
  %b = getelementptr inbounds i8, ptr %a, i64 8
  %va = load i32, ptr %a, align 16
  %vb = load i32, ptr %b, align 8
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, <4 x i1>, <4 x i32>)
