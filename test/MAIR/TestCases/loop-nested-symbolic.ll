; RUN: mair-opt -passes=lpoc %s | FileCheck %s
; RUN: mair-opt -passes=lpoc,liom %s | FileCheck %s --check-prefix=HOIST
; RUN: mair-opt -passes=lpoc --disable-output %s

; An inner loop's range whose size is an expression, or its periodic region,
; folds into one range of the parent loop when the parent's iterations abut
; (LoopPeriodicObservationCombinePass.cpp, buildNested*Aggregate). The
; negative cases keep the inner summary in the parent loop.

; for (i < m) for (j < n) read p[i * n + j]: row i is [p + 4ni, + 4n), and the
; i loop advances it by 4n, its size.
define void @rows_abut(ptr %p, i64 %n, i64 %m) {
; CHECK-LABEL: mair graph of fn @rows_abut {
; CHECK:       loop-exit=%i.header,
; CHECK-NEXT:    o: read ptr %p, ({{.*}}%m{{.*}} * {{.*}}%n{{.*}}), align 4
; CHECK-LABEL: mair graph of fn @rows_abut of loop %i.header {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %enter = icmp sgt i64 %m, 0
  %nonempty = icmp sgt i64 %n, 0
  %go = and i1 %enter, %nonempty
  br i1 %go, label %i.header, label %exit
i.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %i.latch ]
  %row.index = mul nuw nsw i64 %i, %n
  %row = getelementptr inbounds float, ptr %p, i64 %row.index
  br label %j.body
j.body:
  %j = phi i64 [ 0, %i.header ], [ %j.next, %j.body ]
  %addr = getelementptr inbounds float, ptr %row, i64 %j
  %v = load float, ptr %addr, align 4
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %n
  br i1 %j.done, label %i.latch, label %j.body
i.latch:
  %i.next = add nuw nsw i64 %i, 1
  %i.done = icmp eq i64 %i.next, %m
  br i1 %i.done, label %exit, label %i.header
exit:
  ret void
}

; Rows of n elements a pitch apart leave a gap unless pitch == n, which is
; not known: the rows stay in the i loop.
define void @rows_with_pitch(ptr %p, i64 %n, i64 %m, i64 %pitch) {
; CHECK-LABEL: mair graph of fn @rows_with_pitch {
; CHECK-NOT:   o:
; CHECK-LABEL: mair graph of fn @rows_with_pitch of loop %i.header {
; CHECK:       loop-exit=%j.body,
; CHECK-NEXT:    o: read ptr %p + %row, (4 + (4 * zext((18446744073709551615 + %n) to i67))), align 4
; CHECK-LABEL: mair graph of fn @rows_with_pitch of loop %j.body {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %enter = icmp sgt i64 %m, 0
  %nonempty = icmp sgt i64 %n, 0
  %go = and i1 %enter, %nonempty
  br i1 %go, label %i.header, label %exit
i.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %i.latch ]
  %row.index = mul nuw nsw i64 %i, %pitch
  %row = getelementptr inbounds float, ptr %p, i64 %row.index
  br label %j.body
j.body:
  %j = phi i64 [ 0, %i.header ], [ %j.next, %j.body ]
  %addr = getelementptr inbounds float, ptr %row, i64 %j
  %v = load float, ptr %addr, align 4
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %n
  br i1 %j.done, label %i.latch, label %j.body
i.latch:
  %i.next = add nuw nsw i64 %i, 1
  %i.done = icmp eq i64 %i.next, %m
  br i1 %i.done, label %exit, label %i.header
exit:
  ret void
}

; for (i < m) for (j < len[i]) read p[i * n + j]: the row size 4 * len[i]
; changes with i, so it is not one stride of the i loop.
define void @rows_ragged(ptr %p, ptr %len, i64 %n, i64 %m) {
; CHECK-LABEL: mair graph of fn @rows_ragged {
; CHECK-NOT:   o: read ptr %p
; CHECK-LABEL: mair graph of fn @rows_ragged of loop %i.header {
; CHECK:         o: read ptr %p + %row, (4 + (4 * zext((18446744073709551615 + %row.len) to i67))), align 4
; CHECK-LABEL: mair graph of fn @rows_ragged of loop %j.body {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %enter = icmp sgt i64 %m, 0
  br i1 %enter, label %i.header, label %exit
i.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %i.latch ]
  %row.index = mul nuw nsw i64 %i, %n
  %row = getelementptr inbounds float, ptr %p, i64 %row.index
  %len.addr = getelementptr inbounds i64, ptr %len, i64 %i
  %row.len = load i64, ptr %len.addr, align 8
  br label %j.body
j.body:
  %j = phi i64 [ 0, %i.header ], [ %j.next, %j.body ]
  %addr = getelementptr inbounds float, ptr %row, i64 %j
  %v = load float, ptr %addr, align 4
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %row.len
  br i1 %j.done, label %i.latch, label %j.body
i.latch:
  %i.next = add nuw nsw i64 %i, 1
  %i.done = icmp eq i64 %i.next, %m
  br i1 %i.done, label %exit, label %i.header
exit:
  ret void
}

; for (j < n) for (k < n) read B[k * n + j]: column j is periodic (start
; B + 4j, stride 4n, access size 4, n trips). The j loop advances the start
; by 4, the access size, and n * 4 is the stride, so the columns fill
; [B, + 4n^2).
define void @columns(ptr %B, i64 %n) {
; CHECK-LABEL: mair graph of fn @columns {
; CHECK:       loop-exit=%j.header,
; CHECK-NEXT:    o: read ptr %B, ({{.*}}%n{{.*}}), align 4
; CHECK-LABEL: mair graph of fn @columns of loop %j.header {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %enter = icmp sgt i64 %n, 0
  br i1 %enter, label %j.header, label %exit
j.header:
  %j = phi i64 [ 0, %entry ], [ %j.next, %j.latch ]
  %col = getelementptr float, ptr %B, i64 %j
  br label %k.body
k.body:
  %k = phi i64 [ 0, %j.header ], [ %k.next, %k.body ]
  %index = mul nuw nsw i64 %k, %n
  %addr = getelementptr float, ptr %col, i64 %index
  %v = load float, ptr %addr, align 4
  %k.next = add nuw nsw i64 %k, 1
  %k.done = icmp eq i64 %k.next, %n
  br i1 %k.done, label %j.latch, label %k.body
j.latch:
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %n
  br i1 %j.done, label %exit, label %j.header
exit:
  ret void
}

; Only the first m columns: m * 4 is not known to be the stride 4n, so the
; columns leave gaps and stay periodic in the j loop.
define void @columns_partial(ptr %B, i64 %n, i64 %m) {
; CHECK-LABEL: mair graph of fn @columns_partial {
; CHECK-NOT:   o:
; CHECK-LABEL: mair graph of fn @columns_partial of loop %j.header {
; CHECK:         o: read period start=%col, stride=(4 * %n), access-size=4, trips=
; CHECK-LABEL: mair graph of fn @columns_partial of loop %k.body {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %enter = icmp sgt i64 %m, 0
  %nonempty = icmp sgt i64 %n, 0
  %go = and i1 %enter, %nonempty
  br i1 %go, label %j.header, label %exit
j.header:
  %j = phi i64 [ 0, %entry ], [ %j.next, %j.latch ]
  %col = getelementptr float, ptr %B, i64 %j
  br label %k.body
k.body:
  %k = phi i64 [ 0, %j.header ], [ %k.next, %k.body ]
  %index = mul nuw nsw i64 %k, %n
  %addr = getelementptr float, ptr %col, i64 %index
  %v = load float, ptr %addr, align 4
  %k.next = add nuw nsw i64 %k, 1
  %k.done = icmp eq i64 %k.next, %n
  br i1 %k.done, label %j.latch, label %k.body
j.latch:
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %m
  br i1 %j.done, label %exit, label %j.header
exit:
  ret void
}

; Every other column, B[k * n + 2j]: the j loop advances the start by 8, more
; than the access size, so the columns leave gaps.
define void @columns_step2(ptr %B, i64 %n) {
; CHECK-LABEL: mair graph of fn @columns_step2 {
; CHECK-NOT:   o:
; CHECK-LABEL: mair graph of fn @columns_step2 of loop %j.header {
; CHECK:         o: read period start=%col, stride=(4 * %n), access-size=4, trips=
; CHECK-LABEL: mair graph of fn @columns_step2 of loop %k.body {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %enter = icmp sgt i64 %n, 0
  br i1 %enter, label %j.header, label %exit
j.header:
  %j = phi i64 [ 0, %entry ], [ %j.next, %j.latch ]
  %j2 = shl nuw nsw i64 %j, 1
  %col = getelementptr float, ptr %B, i64 %j2
  br label %k.body
k.body:
  %k = phi i64 [ 0, %j.header ], [ %k.next, %k.body ]
  %index = mul nuw nsw i64 %k, %n
  %addr = getelementptr float, ptr %col, i64 %index
  %v = load float, ptr %addr, align 4
  %k.next = add nuw nsw i64 %k, 1
  %k.done = icmp eq i64 %k.next, %n
  br i1 %k.done, label %j.latch, label %k.body
j.latch:
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %n
  br i1 %j.done, label %exit, label %j.header
exit:
  ret void
}

; Columns of B + 4, B[k * n + j + 1]: no IR value computes the start
; B + 4 + 4j, so the periodic region starts at %col plus the distance 4. The
; j loop folds the columns into [B + 4, + 4n^2).
define void @columns_from_distance(ptr %B, i64 %n) {
; CHECK-LABEL: mair graph of fn @columns_from_distance {
; CHECK:       loop-exit=%j.header,
; CHECK-NEXT:    o: read ptr %B + 4, ({{.*}}%n{{.*}}), align 4
; CHECK-LABEL: mair graph of fn @columns_from_distance of loop %j.header {
; CHECK-NOT:   o:
; CHECK:       }
entry:
  %enter = icmp sgt i64 %n, 0
  br i1 %enter, label %j.header, label %exit
j.header:
  %j = phi i64 [ 0, %entry ], [ %j.next, %j.latch ]
  %col = getelementptr float, ptr %B, i64 %j
  br label %k.body
k.body:
  %k = phi i64 [ 0, %j.header ], [ %k.next, %k.body ]
  %index = mul nuw nsw i64 %k, %n
  %index.1 = add nuw nsw i64 %index, 1
  %addr = getelementptr float, ptr %col, i64 %index.1
  %v = load float, ptr %addr, align 4
  %k.next = add nuw nsw i64 %k, 1
  %k.done = icmp eq i64 %k.next, %n
  br i1 %k.done, label %j.latch, label %k.body
j.latch:
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %n
  br i1 %j.done, label %exit, label %j.header
exit:
  ret void
}

; The same columns read in every iteration of an outer loop: the folded range
; does not depend on i, and LIOM hoists it (size expression included) out of
; the i loop.
define void @columns_invariant(ptr %B, i64 %n, i64 %m) {
; HOIST-LABEL: mair graph of fn @columns_invariant {
; HOIST:       loop-preheader=%i.header,
; HOIST-NEXT:    o: read ptr %B, ({{.*}}%n{{.*}}), align 4
; HOIST-LABEL: mair graph of fn @columns_invariant of loop %i.header {
; HOIST-NOT:   o:
; HOIST:       }
entry:
  %enter = icmp sgt i64 %m, 0
  %nonempty = icmp sgt i64 %n, 0
  %go = and i1 %enter, %nonempty
  br i1 %go, label %i.header, label %exit
i.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %i.latch ]
  br label %j.header
j.header:
  %j = phi i64 [ 0, %i.header ], [ %j.next, %j.latch ]
  %col = getelementptr float, ptr %B, i64 %j
  br label %k.body
k.body:
  %k = phi i64 [ 0, %j.header ], [ %k.next, %k.body ]
  %index = mul nuw nsw i64 %k, %n
  %addr = getelementptr float, ptr %col, i64 %index
  %v = load float, ptr %addr, align 4
  %k.next = add nuw nsw i64 %k, 1
  %k.done = icmp eq i64 %k.next, %n
  br i1 %k.done, label %j.latch, label %k.body
j.latch:
  %j.next = add nuw nsw i64 %j, 1
  %j.done = icmp eq i64 %j.next, %n
  br i1 %j.done, label %i.latch, label %j.header
i.latch:
  %i.next = add nuw nsw i64 %i, 1
  %i.done = icmp eq i64 %i.next, %m
  br i1 %i.done, label %exit, label %i.header
exit:
  ret void
}
