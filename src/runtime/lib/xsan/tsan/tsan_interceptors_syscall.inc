/// This code is copied from tsan_interceptors_posix.cpp

namespace __tsan {
  struct __tsan::ThreadState *thr = __tsan::cur_thread();;
  void Acquire(ThreadState *thr, uptr pc, uptr addr);
  void Release(ThreadState *thr, uptr pc, uptr addr);
  void FdClose(ThreadState *thr, uptr pc, int fd, bool write);
  void FdAcquire(ThreadState *thr, uptr pc, int fd);
  void FdRelease(ThreadState *thr, uptr pc, int fd);
  void FdOnFork(ThreadState *thr, uptr pc);
  void ForkBefore(ThreadState *thr, uptr pc);
  void ForkChildAfter(ThreadState *thr, uptr pc, bool start_thread);
  void ForkParentAfter(ThreadState *thr, uptr pc);
  void EnterBlockingFunc(ThreadState *thr);
} // namespace __tsan

static USED void syscall_acquire(uptr pc, uptr addr) {
  XSAN_SYSCALL();
  __tsan::Acquire(__tsan::cur_thread(), pc, addr);
  DPrintf("syscall_acquire(0x%zx))\n", addr);
}

static USED void syscall_release(uptr pc, uptr addr) {
  XSAN_SYSCALL();
  DPrintf("syscall_release(0x%zx)\n", addr);
  __tsan::Release(__tsan::cur_thread(), pc, addr);
}

static void syscall_fd_close(uptr pc, int fd) {
  __tsan::FdClose(__tsan::cur_thread(), pc, fd, true);
}

static USED void syscall_fd_acquire(uptr pc, int fd) {
  XSAN_SYSCALL();
  __tsan::FdAcquire(__tsan::cur_thread(), pc, fd);
  DPrintf("syscall_fd_acquire(%d)\n", fd);
}

static USED void syscall_fd_release(uptr pc, int fd) {
  XSAN_SYSCALL();
  DPrintf("syscall_fd_release(%d)\n", fd);
  __tsan::FdRelease(__tsan::cur_thread(), pc, fd);
}

static void syscall_pre_fork(uptr pc) {
  __tsan::ForkBefore(__tsan::cur_thread(), pc);
}

static void syscall_post_fork(uptr pc, int pid) {
  __tsan::ThreadState *thr = __tsan::cur_thread();
  if (pid == 0) {
    // child
    __tsan::ForkChildAfter(thr, pc, true);
    __tsan::FdOnFork(thr, pc);
  } else if (pid > 0) {
    // parent
    __tsan::ForkParentAfter(thr, pc);
  } else {
    // error
    __tsan::ForkParentAfter(thr, pc);
  }
}

static USED void sycall_blocking_start() {
  DPrintf("sycall_blocking_start()\n");
  __tsan::ThreadState *thr = __tsan::cur_thread();
  __tsan::EnterBlockingFunc(thr);
  // When we are in a "blocking call", we process signals asynchronously
  // (right when they arrive). In this context we do not expect to be
  // executing any user/runtime code. The known interceptor sequence when
  // this is not true is: pthread_join -> munmap(stack). It's fine
  // to ignore munmap in this case -- we handle stack shadow separately.
  thr->ignore_interceptors++;
}

static USED void sycall_blocking_end() {
  DPrintf("sycall_blocking_end()\n");
  __tsan::ThreadState *thr = __tsan::cur_thread();
  thr->ignore_interceptors--;
  atomic_store(&thr->in_blocking_func, 0, memory_order_relaxed);
}


#define COMMON_SYSCALL_ACQUIRE(addr) \
      syscall_acquire(GET_CALLER_PC(), (uptr)(addr))

#define COMMON_SYSCALL_RELEASE(addr) \
      syscall_release(GET_CALLER_PC(), (uptr)(addr))

#define COMMON_SYSCALL_FD_CLOSE(fd) syscall_fd_close(GET_CALLER_PC(), fd)

#define COMMON_SYSCALL_FD_ACQUIRE(fd) \
      syscall_fd_acquire(GET_CALLER_PC(), fd)

#define COMMON_SYSCALL_FD_RELEASE(fd) \
      syscall_fd_release(GET_CALLER_PC(), fd)

#define COMMON_SYSCALL_PRE_FORK() syscall_pre_fork(GET_CALLER_PC())

#define COMMON_SYSCALL_POST_FORK(res) \
      syscall_post_fork(GET_CALLER_PC(), res)

#define COMMON_SYSCALL_BLOCKING_START() sycall_blocking_start()
#define COMMON_SYSCALL_BLOCKING_END() sycall_blocking_end()
