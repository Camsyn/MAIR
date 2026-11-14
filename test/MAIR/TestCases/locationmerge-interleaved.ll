; Periodic regions with the same base, access size, trip count and constant
; stride S whose starts are equally spaced by |S| / m interleave into one
; periodic region: stride S / m, m times the trips, and the lowest start (the
; highest for a negative stride). The trip count is widened so that m times it
; cannot wrap.
; RUN: mair-opt -passes="lpoc,locationmerge" %s | FileCheck %s
; RUN: mair-opt -passes="lpoc,locationmerge" --disable-output %s

; Two gathered lanes of a 32-byte stride: +0 and +16, size 4.
define void @gather_pair(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @gather_pair {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=16, access-size=4, trips=(2 * zext((1 + {{.*}}) to i66))
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nuw nsw i64 %i, 32
  %a = getelementptr inbounds i8, ptr %p, i64 %i.32
  %b = getelementptr inbounds i8, ptr %a, i64 16
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A constant trip count folds: 2 * 100 trips.
define void @constant_trips(ptr %p) {
; CHECK-LABEL: mair graph of fn @constant_trips {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=16, access-size=4, trips=200
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nuw nsw i64 %i, 32
  %a = getelementptr inbounds i8, ptr %p, i64 %i.32
  %b = getelementptr inbounds i8, ptr %a, i64 16
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, 100
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Three slices of a 24-byte stride: +0, +8, +16, size 4.
define void @three_way(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @three_way {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=8, access-size=4, trips=(3 * zext((1 + {{.*}}) to i67))
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.24 = mul nuw nsw i64 %i, 24
  %a = getelementptr inbounds i8, ptr %p, i64 %i.24
  %b = getelementptr inbounds i8, ptr %a, i64 8
  %c = getelementptr inbounds i8, ptr %a, i64 16
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %vc = load i32, ptr %c, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Four slices of a 64-byte stride, listed out of order: +32, +0, +48, +16.
define void @four_way(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @four_way {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: write period start=%p, stride=16, access-size=4, trips=(4 * zext((1 + {{.*}}) to i67))
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.64 = mul nuw nsw i64 %i, 64
  %a = getelementptr inbounds i8, ptr %p, i64 %i.64
  %b = getelementptr inbounds i8, ptr %a, i64 16
  %c = getelementptr inbounds i8, ptr %a, i64 32
  %d = getelementptr inbounds i8, ptr %a, i64 48
  store i32 0, ptr %c, align 4
  store i32 0, ptr %a, align 4
  store i32 0, ptr %d, align 4
  store i32 0, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A negative stride: the merged region starts at the highest start and steps
; down by 16.
define void @negative_stride(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @negative_stride {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=(%p + 16), stride=-16, access-size=4, trips=(2 * zext((1 + {{.*}}) to i66))
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nsw i64 %i, -32
  %a = getelementptr inbounds i8, ptr %p, i64 %i.32
  %b = getelementptr inbounds i8, ptr %a, i64 16
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; +0 and +12 of a 32-byte stride: 12 does not divide 32.
define void @unequal_spacing(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @unequal_spacing {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=32, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: read period start=(%p + 12), stride=32, access-size=4, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nuw nsw i64 %i, 32
  %a = getelementptr inbounds i8, ptr %p, i64 %i.32
  %b = getelementptr inbounds i8, ptr %a, i64 12
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; +0, +8 and +20 of a 32-byte stride: no equally spaced subset covers a
; whole stride.
define void @uneven_three(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @uneven_three {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=32, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: read period start=(%p + 8), stride=32, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: read period start=(%p + 20), stride=32, access-size=4, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nuw nsw i64 %i, 32
  %a = getelementptr inbounds i8, ptr %p, i64 %i.32
  %b = getelementptr inbounds i8, ptr %a, i64 8
  %c = getelementptr inbounds i8, ptr %a, i64 20
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %vc = load i32, ptr %c, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; +0 and +16 of a 48-byte stride: two slices 16 apart cover only two thirds
; of the stride (m * g != |S|).
define void @partial_stride(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @partial_stride {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=48, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: read period start=(%p + 16), stride=48, access-size=4, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.48 = mul nuw nsw i64 %i, 48
  %a = getelementptr inbounds i8, ptr %p, i64 %i.48
  %b = getelementptr inbounds i8, ptr %a, i64 16
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Access sizes 4 and 8 at +0 and +16 of a 32-byte stride.
define void @different_sizes(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @different_sizes {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=32, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: read period start=(%p + 16), stride=32, access-size=8, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nuw nsw i64 %i, 32
  %a = getelementptr inbounds i8, ptr %p, i64 %i.32
  %b = getelementptr inbounds i8, ptr %a, i64 16
  %va = load i32, ptr %a, align 4
  %vb = load i64, ptr %b, align 8
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; The load before the exit test runs once more than the one after it, so
; the two regions have different trip counts.
define void @different_trips(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @different_trips {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=32, access-size=4, trips=(1 + zext(%n to i65))
; CHECK-NEXT:    o: read period start=(%p + 16), stride=32, access-size=4, trips=zext(%n to i65)
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %i.32 = mul nuw nsw i64 %i, 32
  %a = getelementptr inbounds i8, ptr %p, i64 %i.32
  %va = load i32, ptr %a, align 4
  %done = icmp eq i64 %i, %n
  br i1 %done, label %exit, label %latch

latch:
  %b = getelementptr inbounds i8, ptr %a, i64 16
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  br label %loop

exit:
  ret void
}
