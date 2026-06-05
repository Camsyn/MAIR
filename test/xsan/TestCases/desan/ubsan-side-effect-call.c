// RUN: %clang -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -emit-llvm -S %s -o %t.raw.ll
// RUN: %xsan_opt -passes=xsan -xsan-sanitizers=asan,tsan,msan -xsan-de-ubsan -xsan-dump=%t.dump -S %t.raw.ll -o %t.out.ll
// RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/02-desan.ll
// RUN: FileCheck %s --check-prefix=FINAL --input-file=%t.dump/10-final.ll

int foo(void);

void bar(void) {
  int num = foo() + 1;
  (void)num;
}

// DESAN-LABEL: define {{.*}}void @bar(
// DESAN: call {{.*}}@foo()
// DESAN: ret void

// FINAL-LABEL: define {{.*}}void @bar(
// FINAL: call {{.*}}@foo()
// FINAL: ret void
