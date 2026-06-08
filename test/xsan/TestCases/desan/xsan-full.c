// Verify the composed XSan DeSan/ReSan flow.  The test only checks the final
// DeSan snapshot and the immediately-after-ReSan snapshot.
// RUN: %clang -O0 -g -S -emit-llvm %s -o %t.raw.ll
// RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-dump=%t.dump -S %t.raw.ll -o %t.out.ll
// RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/06-desan.ll
// RUN: FileCheck %s --check-prefix=RESAN --input-file=%t.dump/07-resan.ll

struct Big {
  long a[8];
};

int G;

__attribute__((noinline)) long full_xsan(struct Big b, long *p, int c) {
  int x;
  if (c)
    G++;
  return b.a[0] + *p + x + G;
}

// DESAN-LABEL: define dso_local i64 @full_xsan(
// DESAN-NOT: !xsan.asan
// DESAN-NOT: !xsan.tsan
// DESAN-NOT: !xsan.msan
// DESAN-NOT: @__asan_report_load8
// DESAN-NOT: @__tsan_
// DESAN-NOT: @__msan_
// DESAN: ret i64
// DESAN: }

// RESAN-LABEL: define dso_local i64 @full_xsan(
// RESAN-DAG: call void @__tsan_func_entry{{.*}}!xsan.tsan
// RESAN-DAG: call void @llvm.memset.p0.i64{{.*}}!xsan.msan
// RESAN-DAG: call void @llvm.memcpy.p0.p0.i64{{.*}}!xsan.asan
// RESAN-DAG: call void @__tsan_write4{{.*}}!xsan.tsan
// RESAN-DAG: call void @__asan_report_load8{{.*}}!xsan.asan
// RESAN-DAG: call void @__msan_warning_noreturn{{.*}}!xsan.msan
// RESAN: }
