// clang-format off
// Minimal test for LeakSanitizer+{{AddressSanitizer|XSan}}.
// REQUIRES: leak-detection
//
// This test intentionally leaks `new int[] - 100000`. LSan reports a leak only
// if no stale copy of the real heap pointer remains in stack/register roots.
// Thread/undefined sanitizer instrumentation can split the producer and use into
// different basic blocks; at -O0, ISel may then spill the returned `new` pointer
// to the stack and make LSan treat the allocation as reachable. Disable those
// sanitizers so the allocation and its use stay in one basic block.
// RUN: %clangxx_asan -fno-sanitize=thread,undefined %s -o %t
// RUN: %env_asan_opts=detect_leaks=1 not %run %t  2>&1 | FileCheck %s
// RUN: not %run %t  2>&1 | FileCheck %s
// RUN: %env_asan_opts=detect_leaks=0     %run %t
//
// clang-format on
#include <stdio.h>
int *t;

int main(int argc, char **argv) {
  t = new int[argc - 1] - 100000;
  printf("t: %p\n", t);
}
// CHECK: LeakSanitizer: detected memory leaks
