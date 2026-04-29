// Verify the single-sanitizer DeSan/ReSan flow for ASan:
// Raw IR -> ASan -> DeSan -> ReSan.
//
// The by-value aggregate argument makes ASan create an Arg0.byval shadow stack
// copy.  DeSan must restore the original user argument, and ReSan must replay
// the ASan replacement without corrupting the memcpy source (%0).
// RUN: %clang -O0 -g -S -emit-llvm %s -o %t.raw.ll
// RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan -dump-desan-resan=%t.dump -S %t.raw.ll -o %t.out.ll
// RUN: FileCheck %s --check-prefix=ASAN --input-file=%t.dump/03-asan.ll
// RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/04-desan.ll
// RUN: FileCheck %s --check-prefix=RESAN --input-file=%t.dump/05-resan.ll

struct Big {
  long a[8];
};

int Global[4];

__attribute__((noinline)) long byval_arg_replace(struct Big b, long *p,
                                                 int idx) {
  long local[4];
  int small = idx & 3;
  int large = idx & 7;
  local[small] = b.a[large];
  Global[small] = (int)*p;
  return local[(small + 1) & 3] + Global[small];
}

// ASAN: Arg0.byval
// ASAN-LABEL: define dso_local i64 @byval_arg_replace(
// ASAN: call void @llvm.memcpy.p0.p0.i64(ptr align 8 [[COPY:%[0-9]+]], ptr align 8 %0, i64 64, i1 false)
// ASAN: getelementptr inbounds {{.*}}, ptr [[COPY]],
// ASAN: call void @__asan_report_load8
// ASAN: }

// DESAN-LABEL: define dso_local i64 @byval_arg_replace(
// DESAN-NOT: !xsan.asan
// DESAN-NOT: @__asan_report_load8
// DESAN: getelementptr inbounds {{.*}}, ptr %0,
// DESAN: ret i64
// DESAN: }

// RESAN-LABEL: define dso_local i64 @byval_arg_replace(
// RESAN: call void @llvm.memcpy.p0.p0.i64(ptr align 8 [[COPY:%[0-9]+]], ptr align 8 %0, i64 64, i1 false), {{.*}}!xsan.asan
// RESAN: getelementptr inbounds {{.*}}, ptr [[COPY]],
// RESAN: call void @__asan_report_load8
// RESAN: !xsan.asan
// RESAN: }
