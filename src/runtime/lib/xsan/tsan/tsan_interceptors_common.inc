/// This code is copied from tsan_interceptors_posix.cpp

namespace __tsan {
struct ThreadState;
SANITIZER_WEAK_CXX_DEFAULT_IMPL
uptr Dir2addr(const char *path);
uptr File2addr(const char *path);
void Acquire(ThreadState *thr, uptr pc, uptr addr);
void Release(ThreadState *thr, uptr pc, uptr addr);

void FdAcquire(ThreadState *thr, uptr pc, int fd);
void FdRelease(ThreadState *thr, uptr pc, int fd);
void FdAccess(ThreadState *thr, uptr pc, int fd);
void FdClose(ThreadState *thr, uptr pc, int fd, bool write);
void FdFileCreate(ThreadState *thr, uptr pc, int fd);
void FdSocketAccept(ThreadState *thr, uptr pc, int fd, int newfd);
void MemoryRangeImitateWriteOrResetRange(ThreadState *thr, uptr pc, uptr addr,
                                         uptr size);

void HandleRecvmsg(ThreadState *thr, uptr pc, __sanitizer_msghdr *msg);
}  // namespace __tsan

#define TSAN_THR ((const XsanInterceptorContext *)ctx)->xsan_ctx.tsan.thr_
#define TSAN_PC ((const XsanInterceptorContext *)ctx)->xsan_ctx.tsan.pc_

#define COMMON_INTERCEPTOR_BLOCK_REAL(name) \
  (__tsan::BlockingCall(xsan_ctx.tsan.thr_), REAL(name))

#define COMMON_INTERCEPTOR_FILE_OPEN(ctx, file, path)            \
  if (path) {                                                    \
    __tsan::Acquire(TSAN_THR, TSAN_PC, __tsan::File2addr(path)); \
  }                                                              \
  if (file) {                                                    \
    int fd = fileno_unlocked(file);                              \
    if (fd >= 0) {                                               \
      __tsan::FdFileCreate(TSAN_THR, TSAN_PC, fd);               \
    }                                                            \
  }
#define COMMON_INTERCEPTOR_FILE_CLOSE(ctx, file) \
  if (file) {                                    \
    int fd = fileno_unlocked(file);              \
    __tsan::FdClose(TSAN_THR, TSAN_PC, fd, true);      \
  }
#define COMMON_INTERCEPTOR_ACQUIRE(ctx, u) \
  __tsan::Acquire(TSAN_THR, TSAN_PC, u);
#define COMMON_INTERCEPTOR_RELEASE(ctx, u) \
  __tsan::Release(TSAN_THR, TSAN_PC, u);
#define COMMON_INTERCEPTOR_DIR_ACQUIRE(ctx, path) \
  __tsan::Acquire(TSAN_THR, TSAN_PC, __tsan::Dir2addr(path));
#define COMMON_INTERCEPTOR_FD_ACQUIRE(ctx, fd) \
  __tsan::FdAcquire(TSAN_THR, TSAN_PC, fd);
#define COMMON_INTERCEPTOR_FD_RELEASE(ctx, fd) \
  __tsan::FdRelease(TSAN_THR, TSAN_PC, fd);
#define COMMON_INTERCEPTOR_FD_ACCESS(ctx, fd) \
  __tsan::FdAccess(TSAN_THR, TSAN_PC, fd);
#define COMMON_INTERCEPTOR_FD_SOCKET_ACCEPT(ctx, fd, newfd) \
  __tsan::FdSocketAccept(TSAN_THR, TSAN_PC, fd, newfd);
#define COMMON_INTERCEPTOR_USER_CALLBACK_START() xsi.DisableIgnores();
#define COMMON_INTERCEPTOR_USER_CALLBACK_END() xsi.EnableIgnores();
#if !SANITIZER_APPLE
#  define COMMON_INTERCEPTOR_HANDLE_RECVMSG(ctx, msg) \
    __tsan::HandleRecvmsg(TSAN_THR, TSAN_PC, msg);
#endif
