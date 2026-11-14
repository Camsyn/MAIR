; Periodic regions with the same base, stride and trip count whose footprints
; touch or overlap merge into one periodic region covering exactly their union.
; RUN: mair-opt -passes="lpoc,locationmerge" %s | FileCheck %s
; RUN: mair-opt -passes="lpoc,locationmerge" --disable-output %s

; s[i].a and s[i].b of a 12-byte struct: start +0 size 4 and start +4 size 4.
define void @adjacent_fields(ptr %s, i64 %n) {
; CHECK-LABEL: mair graph of fn @adjacent_fields {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%s, stride=12, access-size=8, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
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

; The higher region starts inside the lower one: +0 size 4 and +2 size 4.
define void @overlapping(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @overlapping {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=16, access-size=6, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.16 = mul nuw nsw i64 %i, 16
  %lo = getelementptr inbounds i8, ptr %p, i64 %i.16
  %hi = getelementptr inbounds i8, ptr %lo, i64 2
  %vlo = load i32, ptr %lo, align 4
  %vhi = load i32, ptr %hi, align 2
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Listed high first: the merged region still starts at the lower start.
define void @higher_listed_first(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @higher_listed_first {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=16, access-size=8, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.16 = mul nuw nsw i64 %i, 16
  %lo = getelementptr inbounds i8, ptr %p, i64 %i.16
  %hi = getelementptr inbounds i8, ptr %lo, i64 4
  %vhi = load i32, ptr %hi, align 4
  %vlo = load i32, ptr %lo, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Two vector halves of a 32-byte stride: together they cover every byte, so
; the merged region is contiguous (stride equals access size).
define void @vector_halves(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @vector_halves {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: write period start=%p, stride=32, access-size=32, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nuw nsw i64 %i, 32
  %lo = getelementptr inbounds i8, ptr %p, i64 %i.32
  %hi = getelementptr inbounds i8, ptr %lo, i64 16
  store <4 x i32> zeroinitializer, ptr %lo, align 16
  store <4 x i32> zeroinitializer, ptr %hi, align 16
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Three regions where only the middle one bridges the other two.
define void @bridged(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @bridged {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=32, access-size=12, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.32 = mul nuw nsw i64 %i, 32
  %p0 = getelementptr inbounds i8, ptr %p, i64 %i.32
  %p8 = getelementptr inbounds i8, ptr %p0, i64 8
  %p4 = getelementptr inbounds i8, ptr %p0, i64 4
  %v0 = load i32, ptr %p0, align 4
  %v8 = load i32, ptr %p8, align 4
  %v4 = load i32, ptr %p4, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A gap between the footprints: the union is not one periodic region.
define void @gap(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @gap {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=16, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: read period start=(%p + 8), stride=16, access-size=4, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.16 = mul nuw nsw i64 %i, 16
  %lo = getelementptr inbounds i8, ptr %p, i64 %i.16
  %hi = getelementptr inbounds i8, ptr %lo, i64 8
  %vlo = load i32, ptr %lo, align 4
  %vhi = load i32, ptr %hi, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Same base, different strides.
define void @different_strides(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @different_strides {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=8, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: read period start=(%p + 4), stride=12, access-size=4, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.8 = mul nuw nsw i64 %i, 8
  %i.12 = mul nuw nsw i64 %i, 12
  %a = getelementptr inbounds i8, ptr %p, i64 %i.8
  %b.base = getelementptr inbounds i8, ptr %p, i64 %i.12
  %b = getelementptr inbounds i8, ptr %b.base, i64 4
  %va = load i32, ptr %a, align 4
  %vb = load i32, ptr %b, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; The starts share a symbolic part and differ by a constant.
define void @symbolic_start(ptr %p, i64 %k, i64 %n) {
; CHECK-LABEL: mair graph of fn @symbolic_start {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=(%p + %k), stride=16, access-size=8, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  %b = getelementptr inbounds i8, ptr %p, i64 %k
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.16 = mul nuw nsw i64 %i, 16
  %lo = getelementptr inbounds i8, ptr %b, i64 %i.16
  %hi = getelementptr inbounds i8, ptr %lo, i64 4
  %vlo = load i32, ptr %lo, align 4
  %vhi = load i32, ptr %hi, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A read and a write have different levels in the generic configuration, so
; they stay apart, as contiguous accesses do.
define void @different_levels(ptr %p, i64 %n) {
; CHECK-LABEL: mair graph of fn @different_levels {
; CHECK:       loop-exit=%loop,
; CHECK-NEXT:    o: read period start=%p, stride=16, access-size=4, trips={{.*}}
; CHECK-NEXT:    o: write period start=(%p + 4), stride=16, access-size=4, trips={{.*}}
; CHECK-NOT:     o:
; CHECK:       }
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %i.16 = mul nuw nsw i64 %i, 16
  %lo = getelementptr inbounds i8, ptr %p, i64 %i.16
  %hi = getelementptr inbounds i8, ptr %lo, i64 4
  %vlo = load i32, ptr %lo, align 4
  store i32 %vlo, ptr %hi, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
