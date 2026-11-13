// LocationMerge joins adjacent accesses of one program point into a single
// observation. TSan records such an observation as one access of the merged
// bytes instead of one access per source.
// RUN: env MAIR=1 %clang -tsan -O1 -S -emit-llvm %s -o - | FileCheck %s

struct S {
  int x, y;
  long z;
};

// Both fields are written: one 8-byte write. x is 8-byte aligned, so the
// merged access is too.
void write_pair(struct S *p, int a, int b) {
  p->x = a;
  p->y = b;
}
// CHECK-LABEL: define {{.*}} @write_pair(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: call void @__tsan_write8(ptr %p)
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret void

void write_y(struct S *p, int b) { p->y = b; }

// Reads stay reads: one 8-byte read.
int read_pair(struct S *p) { return p->x + p->y; }
// CHECK-LABEL: define {{.*}} @read_pair(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: call void @__tsan_read8(ptr %p)
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret i32

// Three fields cover 16 contiguous bytes.
long read_all(struct S *p) { return p->x + p->y + p->z; }
// CHECK-LABEL: define {{.*}} @read_all(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: call void @__tsan_read16(ptr %p)
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret i64

// a[i] and a[i + 1] have a symbolic offset but a constant size. Only 4-byte
// alignment is known, so the 8-byte write is an unaligned one.
void write_symbolic(int *a, long i) {
  a[i] = 1;
  a[i + 1] = 2;
}
// CHECK-LABEL: define {{.*}} @write_symbolic(
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: call void @__tsan_unaligned_write8(ptr %arrayidx)
// CHECK-NOT: call void @__tsan_{{read|write|unaligned}}
// CHECK: ret void

// x and z leave a gap, so they are never merged into one access: recording y
// would report races on bytes the code never touched.
long read_gap(struct S *p) { return p->x + p->z; }
// CHECK-LABEL: define {{.*}} @read_gap(
// CHECK-DAG: call void @__tsan_read4(ptr %p)
// CHECK-DAG: call void @__tsan_read8(ptr %{{.*}})
// CHECK: ret i64

// A read and a write are never merged; the read is not upgraded to a write.
void copy_x_to_y(struct S *p) { p->y = p->x; }
// CHECK-LABEL: define {{.*}} @copy_x_to_y(
// CHECK-DAG: call void @__tsan_read4(ptr %p)
// CHECK-DAG: call void @__tsan_write4(ptr %{{.*}})
// CHECK: ret void
