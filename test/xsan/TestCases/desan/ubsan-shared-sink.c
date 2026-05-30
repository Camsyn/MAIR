// Verify the UBSan non-dominating shared-sink DeSan/ReSan path inside the
// full xsan pipeline. The source first lets frontend UBSan split multiple
// pointer-overflow checks, then shares a single user sink block after clang
// -O1 optimization.
// RUN: %clang -O1 -fsanitize=undefined -S -emit-llvm %s -o %t.raw.ll
// RUN: %xsan_opt -passes=xsan -xsan-sanitizers=tsan -xsan-dump=%t.dump -S %t.raw.ll -o %t.out.ll
// RUN: FileCheck %s --check-prefix=DESAN --input-file=%t.dump/02-desan.ll
// RUN: FileCheck %s --check-prefix=RESAN --input-file=%t.dump/05-resan.ll

__attribute__((noinline)) void ubsan_shared_sink(char *a, char *b, char *c,
                                                 char **d, int k) {
  char *p;
  switch (k) {
  case 0:
    p = a + 5;
    break;
  case 1:
    p = b + 5;
    break;
  case 2:
    p = c + 5;
    break;
  case 3:
    p = *d + 5;
    break;
  default:
    return;
  }
  (*p)++;
}

// DESAN-LABEL: define dso_local void @ubsan_shared_sink(
// DESAN-NOT: <badref>
// DESAN-NOT: @__ubsan_handle_pointer_overflow
// DESAN-NOT: @__ubsan_handle_type_mismatch_v1
// DESAN: switch i32
// DESAN: phi ptr
// DESAN: load i8
// DESAN: store i8
// DESAN: ret void
// DESAN: }

// RESAN-LABEL: define dso_local void @ubsan_shared_sink(
// RESAN-NOT: <badref>
// RESAN: call void @__tsan_func_entry{{.*}}!xsan.tsan
// RESAN-DAG: tail call void @__ubsan_handle_pointer_overflow{{.*}}!xsan.ubsan
// RESAN-DAG: tail call void @__ubsan_handle_type_mismatch_v1{{.*}}!xsan.ubsan
// RESAN-DAG: call void @__tsan_write1{{.*}}!xsan.tsan
// RESAN-DAG: ret void{{.*}}!xsan.user
// RESAN: }
