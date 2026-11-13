// -mair-stats reports how the sanitizer lowers the observations left after
// optimization: `lowered` ones go through MAIR, `native` ones fall back to the
// sanitizer's own per-access instrumentation. `vector`, `multi` and `symbolic`
// say why an observation fell back; `fallback` counts the source accesses that
// get native checks.
// RUN: env MAIR=1 %clang -asan -O2 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s

typedef int v4 __attribute__((vector_size(16)));
typedef int v4i __attribute__((ext_vector_type(4)));
typedef _Bool v4b __attribute__((ext_vector_type(4)));

// ASan checks a fixed-width vector store as one 16-byte access, so it is
// lowered like a scalar one.
void store_vector(v4 *p, v4 v) { *p = v; }

int load_scalar(int *p) { return *p; }

// Only the lanes a masked load enables are accessed. ASan does not lower that
// region through MAIR; it counts as symbolic.
v4i load_masked(int *p, v4b mask, v4i passthru) {
  return __builtin_masked_load(mask, p, passthru);
}

// CHECK: MAIR stats
// CHECK: select{{ +}}in 3  lowered 2  native 1  symbolic 1  fallback 1{{$}}
