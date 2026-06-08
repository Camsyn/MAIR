// Verify ReSan add replay order when one sanitizer replaces an argument and
// another sanitizer observes that argument in an added sequence.
// RUN: %clang -O0 -g -S -emit-llvm %s -o %t.raw.ll
// RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-dump=%t.dump -S %t.raw.ll -o %t.out.ll
// RUN: FileCheck %s --check-prefix=RESAN --input-file=%t.dump/07-resan.ll

struct Big {
  long a[8];
};

int G;

__attribute__((noinline)) long arg_replacer_order(struct Big b, long *p,
                                                  int c) {
  int x;
  if (c)
    G++;
  return b.a[0] + *p + x + G;
}

// RESAN-LABEL: define dso_local i64 @arg_replacer_order(
// RESAN: call void @__tsan_func_entry{{.*}}!xsan.tsan
// RESAN: call void @llvm.memcpy.p0.p0.i64(ptr align 8 [[COPY:%[0-9]+]], ptr align 8 %0, i64 64, i1 false), {{.*}}!xsan.asan
// RESAN: ptrtoint ptr [[COPY]] to i64, {{.*}}!xsan.msan
// RESAN: call void @llvm.memcpy.p0.p0.i64{{.*}}@__msan_param_tls{{.*}}!xsan.msan
// RESAN: getelementptr inbounds {{.*}}, ptr [[COPY]],
// RESAN: }
