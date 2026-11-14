// A merged observation reports at the source that runs first, not at the
// first source in merge order. LocationMerge orders the sources by offset, so
// the merged read of p->x and p->y lists x first, although y is read first.
// RUN: env MAIR=1 %clang -tsan -O1 -g -S -emit-llvm %s -o - | FileCheck %s

struct S {
  int x, y;
};

int read_y_then_x(struct S *p) {
  int b = p->y; // LINE-Y
  int a = p->x;
  return a * b;
}

// CHECK-LABEL: define {{.*}} @read_y_then_x(
// CHECK: call void @__tsan_{{(unaligned_)?}}read8(ptr %p), !dbg ![[LOC:[0-9]+]]
// CHECK-NOT: call void @__tsan_read
// CHECK: ret i32
// CHECK: ![[LOC]] = !DILocation(line: 11,
