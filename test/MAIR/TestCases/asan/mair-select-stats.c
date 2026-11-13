// -mair-stats reports how the sanitizer lowers the observations left after
// optimization: `lowered` ones go through MAIR, `native` ones fall back to the
// sanitizer's own per-access instrumentation. `vector`, `multi` and `symbolic`
// say why an observation fell back; `fallback` counts the source accesses that
// get native checks.
// RUN: env MAIR=1 %clang -asan -O2 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s

typedef int v4 __attribute__((vector_size(16)));

void store_vector(v4 *p, v4 v) { *p = v; }

int load_scalar(int *p) { return *p; }

// CHECK: MAIR stats
// CHECK: select{{ +}}in 2  lowered 1  native 1  vector 1  fallback 1{{$}}
