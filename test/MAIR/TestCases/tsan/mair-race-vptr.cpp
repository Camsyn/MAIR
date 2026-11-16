// A vtable pointer access keeps its __tsan_vptr_* lowering under MAIR, so a
// race between a virtual call and the vptr update of a destructor is still a
// race on the vptr, even when a field next to the vptr is read with it.
// RUN: env MAIR=1 %clangxx -tsan -O1 -S -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: env MAIR=1 %clangxx -tsan -O1 %s -o %t
// RUN: not %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

// The TSan-invisible barrier of test/tsan/TestCases/test.h: it orders the two
// threads in time without creating a happens-before edge.
extern "C" void __tsan_testonly_barrier_init(unsigned *barrier,
                                             unsigned count);
extern "C" void __tsan_testonly_barrier_wait(unsigned *barrier);
static unsigned barrier;

struct A;
// Opaque to the destructor, so that the vptr update before it is kept.
__attribute__((noinline)) void destroying(A *a) { asm volatile("" ::"r"(a)); }

struct A {
  virtual long F() { return field; }
  virtual ~A() { destroying(this); }
  long field = 1;
};

struct B : A {
  long F() override { return field + 1; }
  ~B() override {}
};

static A *obj = new B;

// The vptr read stays a vptr read and does not merge with the field next to
// it.
// IR-LABEL: define {{.*}} @_Z4callP1A(
// IR: call void @__tsan_vptr_read(
// IR: {{^}$}}
__attribute__((noinline)) long call(A *a) { return a->field + a->F(); }

static void *reader(void *) {
  long v = call(obj);
  __tsan_testonly_barrier_wait(&barrier);
  fprintf(stderr, "READ %ld\n", v);
  return nullptr;
}

int main() {
  __tsan_testonly_barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, nullptr, reader, nullptr);
  __tsan_testonly_barrier_wait(&barrier);
  delete obj;
  pthread_join(t, nullptr);
  fprintf(stderr, "DONE\n");
  return 0;
}
// CHECK: WARNING: ThreadSanitizer: data race on vptr
// CHECK: DONE
