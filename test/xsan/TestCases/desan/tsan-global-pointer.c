// Verify the single-sanitizer DeSan/ReSan flow for TSan:
// Raw IR -> TSan -> DeSan -> ReSan.
//
// The source mixes global and pointer reads/writes across branches so replay
// must preserve function-entry/exit instrumentation and multiple memory access
// callbacks.
// RUN: %clang -O0 -g -S -emit-llvm %s -o %t.raw.ll
// RUN: %xsan_opt -passes=xsan -xsan-sanitizers=tsan -dump-desan-resan=%t.dump -S %t.raw.ll -o %t.out.ll
// RUN: FileCheck %s --check-prefix=TSAN --input-file=%t.dump/03-tsan.ll
// RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/04-desan.ll
// RUN: FileCheck %s --check-prefix=RESAN --input-file=%t.dump/05-resan.ll

int G;

__attribute__((noinline)) int tsan_global_pointer(int *p, int c) {
  if (c) {
    G = *p;
  } else {
    *p = G + 1;
  }
  return G + *p;
}

// TSAN-LABEL: define dso_local i32 @tsan_global_pointer(
// TSAN: call ptr @llvm.returnaddress
// TSAN: call void @__tsan_func_entry
// TSAN: call void @__tsan_read4
// TSAN: call void @__tsan_write4
// TSAN: call void @__tsan_func_exit
// TSAN: }

// DESAN-LABEL: define dso_local i32 @tsan_global_pointer(
// DESAN-NOT: !xsan.tsan
// DESAN-NOT: @__tsan_read4
// DESAN-NOT: @__tsan_write4
// DESAN-NOT: @__tsan_func_entry
// DESAN: ret i32
// DESAN: }

// RESAN-LABEL: define dso_local i32 @tsan_global_pointer(
// RESAN: call ptr @llvm.returnaddress{{.*}}!xsan.tsan
// RESAN: call void @__tsan_func_entry{{.*}}!xsan.tsan
// RESAN: call void @__tsan_read4{{.*}}!xsan.tsan
// RESAN: call void @__tsan_write4{{.*}}!xsan.tsan
// RESAN: call void @__tsan_func_exit{{.*}}!xsan.tsan
// RESAN: }
