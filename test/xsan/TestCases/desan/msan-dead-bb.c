// Verify the single-sanitizer DeSan/ReSan flow for MSan, including the dead
// warning block that MSan inserts for an uninitialized conditional path.
// RUN: %clang -O0 -g -S -emit-llvm %s -o %t.raw.ll
// RUN: %xsan_opt -passes=xsan -xsan-sanitizers=msan -dump-desan-resan=%t.dump -S %t.raw.ll -o %t.out.ll
// RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/04-desan.ll
// RUN: FileCheck %s --check-prefix=RESAN-BB --input-file=%t.dump/05-1-1-resan-add-bb.ll
// RUN: FileCheck %s --check-prefix=RESAN --input-file=%t.dump/05-resan.ll

__attribute__((noinline)) int msan_dead_bb(int c) {
  int x;
  if (c)
    return x;
  return 0;
}

// DESAN-LABEL: define dso_local i32 @msan_dead_bb(
// DESAN-NOT: !xsan.msan
// DESAN-NOT: @__msan_warning_noreturn
// DESAN: ret i32
// DESAN: }

// RESAN-BB-LABEL: define dso_local i32 @msan_dead_bb(
// RESAN-BB: call void @__msan_warning_noreturn() {{.*}}!xsan.msan
// RESAN-BB-NEXT: unreachable, {{.*}}!xsan.msan

// RESAN-LABEL: define dso_local i32 @msan_dead_bb(
// RESAN: call void @llvm.memset.p0.i64{{.*}}!xsan.msan
// RESAN: call void @__msan_warning_noreturn() {{.*}}!xsan.msan
// RESAN: store i32 {{.*}}@__msan_retval_tls{{.*}}!xsan.msan
// RESAN: ret i32
// RESAN: }
