// TSan runs the same full MAIR pipeline as ASan. TSan reports do not need the
// debug-preserving Og pipeline, so a merged or moved observation is fine.
// RUN: env MAIR=1 %clangxx -tsan -O2 -mair-stats -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s

int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i];
  return s;
}

// CHECK: MAIR stats
// CHECK: pipeline O1
// CHECK-NOT: pipeline Og
