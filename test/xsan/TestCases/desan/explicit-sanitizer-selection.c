// XSan's opt entry is intentionally positive-selection only.  Running the
// compositor without -xsan-sanitizers would otherwise silently mean a different
// sanitizer set when new sanitizers are added.
// RUN: %clang -O0 -g -S -emit-llvm %s -o %t.raw.ll
// RUN: not --crash %xsan_opt -passes=xsan -S %t.raw.ll -o %t.out.ll 2>&1 | FileCheck %s

int explicit_selection_required(int *p) { return *p; }

// CHECK: XSan requires explicit -xsan-sanitizers=asan,tsan,msan selection
