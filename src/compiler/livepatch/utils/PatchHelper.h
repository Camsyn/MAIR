// Match functions
#include "debug.h"
#include "xsan_common.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Mutex.h"
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <type_traits>
#include <vector>

namespace __xsan {

bool isXsanEnabled();
const std::string &getStrMask();
const std::bitset<XSan + 1> &getXsanMask();
SanitizerType getSanType();

struct ROSegment {
  char *vaddr;
  size_t memsz;
};

// Get the rodata sections of self module, i.e., ignoring other DSOs.
const std::vector<ROSegment> &getSelfModuleROSegments();

// Memcpy, but can run on address without write permission.
void memcpy_forcibly(void *dst, const void *src, size_t n,
                     bool is_dst_exec = false);

std::string getXsanCombName(const std::bitset<NumSanitizerTypes> &xsan_mask);
std::filesystem::path getThisPatchDsoPath();
std::filesystem::path getXsanAbsPath(std::string_view rel_path);
std::filesystem::path getXsanArchRtDir(std::string_view triple = "");
// Get the executable path of the current process.
std::filesystem::path getSelfPath();
// Return true if the executable file is PIE
bool is_self_proc_pie();
// Return the base address of the executable file
/// TODO: support base address resolution for shared library.
void *get_base_address();

/// TODO: it is too unstable to use name to filter out irrelevant processes.
/// We should find a better way to filter out irrelevant processes.
bool isPatchingProc(const char *proc_name);

// Find the symbol in the main executable or other DSOs.
// 1. Search it by RTLD_NEXT first.
// 2. If not found, try to find it via RTLD_DEFAULT.
void *getRealFuncAddr(const char *ManagledName);
void *getRealFuncAddr(void *InterceptorFunc);

// Find the symbol in the patch DSO.
// 1. Search it by RTLD_DEFAULT first.
// 2. If not found, try to find it via explicit dlopen.
void *getMyFuncAddr(const char *ManagledName);

// Replace the first n instructions to trampoline instructions
class XsanPatch {
#if defined(__x86_64__)
  static constexpr uint8_t PatchTempl[] = {
      // movabs rax, <MyFunc>
      0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
      // jmp rax
      0xff, 0xe0};
  static constexpr size_t FuncAddrOffset = 2; // Offset for MyFunc address
#elif defined(__aarch64__)
  static constexpr uint8_t PatchTempl[] = {
      // ldr x16, 0x8
      0x50, 0x00, 0x00, 0x58,
      // br x16
      0x00, 0x02, 0x1f, 0xd6,
      // .quad <MyFunc>
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr size_t FuncAddrOffset = 8; // Offset for MyFunc address
#elif defined(__loongarch__) && __loongarch_grlen == 64
  static constexpr uint8_t PatchTempl[] = {
      // pcaddu18i $t0, 0
      0x0c, 0x00, 0x00, 0x1e,
      // ld.d $t0, $t0, 12
      0x8c, 0x31, 0xc0, 0x28,
      // jr $t0
      0x80, 0x01, 0x00, 0x4c,
      // .quad <MyFunc>
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr size_t FuncAddrOffset = 12; // Offset for MyFunc address
#elif defined(__riscv) && __riscv_xlen == 64
  static constexpr uint8_t PatchTempl[] = {
      // auipc t0, 0x0
      0x97, 0x02, 0x00, 0x00,
      // ld t0, 10(t0)
      0x83, 0xb2, 0xa2, 0x00,
      // c.jr t0
      0x82, 0x82,
      // .quad <MyFunc>
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr size_t FuncAddrOffset = 10; // Offset for MyFunc address
#else
#error "Unsupported architecture"
#endif
  static constexpr size_t PatchSize = sizeof(PatchTempl);
  using Data = uint8_t[PatchSize];

public:
  XsanPatch() = default;
  XsanPatch(void *MyFunc, void *RealFunc) { initialize(MyFunc, RealFunc); }

  static bool isPatched(void *FuncAddr);

  void initialize(void *MyFunc, void *RealFunc);
  void applyPatch(void *FuncAddr) const;
  void applyBackup(void *FuncAddr) const;

private:
  void validateInit() const;

private:
  bool isInitialized;
  Data patch;
  Data backup;
};

template <typename DeriveTy, typename FunPtr> class XsanInterceptorFunctorBase {
  friend class ScopedCall;

private:
  static void *get(FunPtr fn) {
    void *p;
    if constexpr (sizeof(FunPtr) == sizeof(void *)) {
      // Function pointer is already void *
      p = reinterpret_cast<void *>(fn);
    } else {
      // Method pointer is 16-byte.
      std::memcpy(&p, &fn, sizeof(void *));
    }
    return p;
  }
  static void set(void *p, FunPtr &fn) {
    if constexpr (sizeof(FunPtr) == sizeof(void *)) {
      fn = reinterpret_cast<FunPtr>(p);
    } else {
      std::memset(&fn, 0, sizeof(FunPtr));
      std::memcpy(&fn, &p, sizeof(void *));
    }
  }

protected:
  // Member function pointer may not be the same size as void *
  void *GetMyFunc() const { return get(MyFunc); }
  void *GetRealFunc() const { return get(RealFunc); }
  void SetMyFunc(void *p) { set(p, MyFunc); }
  void SetRealFunc(void *p) { set(p, RealFunc); }

  void PreCall() {}
  void PostCall() {}

  class ScopedCall {
  public:
    ScopedCall(XsanInterceptorFunctorBase *self)
        : Self(static_cast<DeriveTy *>(self)) {
      Self->PreCall();
    }
    ~ScopedCall() { Self->PostCall(); }

  private:
    DeriveTy *Self;
  };

  FunPtr MyFunc;
  FunPtr RealFunc;
};

template <typename DeriveTy, typename FunPtr>
class XsanInterceptorFunctor
    : public XsanInterceptorFunctorBase<DeriveTy, void *> {
protected:
  using ScopedCall =
      typename XsanInterceptorFunctorBase<DeriveTy, void *>::ScopedCall;
};

// Common function pointers (e.g., Ret (*)(Args...))
template <typename DeriveTy, typename Ret, typename... Args>
class XsanInterceptorFunctor<DeriveTy, Ret (*)(Args...)>
    : public XsanInterceptorFunctorBase<DeriveTy, Ret (*)(Args...)> {
protected:
  using ScopedCall =
      typename XsanInterceptorFunctorBase<DeriveTy,
                                          Ret (*)(Args...)>::ScopedCall;

public:
  Ret operator()(Args... args) {
    ScopedCall Scoper(this);
    return (this->RealFunc)(std::forward<Args>(args)...);
  }
};

// Variadic function pointers (e.g., Ret (*)(Args..., ...))
template <typename DeriveTy, typename Ret, typename... Args>
class XsanInterceptorFunctor<DeriveTy, Ret (*)(Args..., ...)>
    : public XsanInterceptorFunctorBase<DeriveTy, Ret (*)(Args..., ...)> {
protected:
  using ScopedCall =
      typename XsanInterceptorFunctorBase<DeriveTy,
                                          Ret (*)(Args..., ...)>::ScopedCall;

public:
  template <typename... CallArgs> Ret operator()(CallArgs &&...args) {
    // Note: we can't do parameter number check here, must ensure that the
    // caller passes the correct parameters to the real function.
    ScopedCall Scoper(this);
    return (this->RealFunc)(std::forward<CallArgs>(args)...);
  }
};

template <typename DeriveTy, typename Ret, typename Class, typename... Args>
class XsanInterceptorFunctor<DeriveTy, Ret (Class::*)(Args...)>
    : public XsanInterceptorFunctorBase<DeriveTy, Ret (Class::*)(Args...)> {
protected:
  using ScopedCall =
      typename XsanInterceptorFunctorBase<DeriveTy,
                                          Ret (Class::*)(Args...)>::ScopedCall;

public:
  Ret operator()(Class *obj, Args... args) {
    ScopedCall Scoper(this);
    return (obj->*(this->RealFunc))(std::forward<Args>(args)...);
  }
};

template <typename FunPtr = void *,
          typename = std::enable_if_t<
              std::is_function_v<std::remove_pointer_t<FunPtr>> ||
              std::is_member_function_pointer_v<FunPtr> ||
              std::is_same_v<FunPtr, void *>>>
class XsanInterceptor
    : public XsanInterceptorFunctor<XsanInterceptor<FunPtr>,
                                    std::remove_cv_t<FunPtr>> {
  using BaseTy =
      XsanInterceptorFunctor<XsanInterceptor<FunPtr>, std::remove_cv_t<FunPtr>>;
  friend BaseTy;
  friend typename BaseTy::ScopedCall;

public:
  XsanInterceptor(FunPtr MyFunc, llvm::ArrayRef<const char *> target_procs)
      : target_procs(target_procs) {
    if (!llvm::any_of(target_procs, isPatchingProc)) {
      // E.g., lld
      return;
    }
    this->MyFunc = MyFunc;
    this->SetRealFunc(getRealFuncAddr(this->GetMyFunc()));
    InitPatch();
  }
  XsanInterceptor(const char *ManagledName,
                  llvm::ArrayRef<const char *> target_procs)
      : target_procs(target_procs) {
    if (!llvm::any_of(target_procs, isPatchingProc)) {
      // E.g., lld
      return;
    }
    this->SetMyFunc(getMyFuncAddr(ManagledName));
    this->SetRealFunc(getRealFuncAddr(ManagledName));
    InitPatch();
  }
  XsanInterceptor(const XsanInterceptor &) = delete;
  XsanInterceptor(XsanInterceptor &&) = delete;
  XsanInterceptor &operator=(const XsanInterceptor &) = delete;
  XsanInterceptor &operator=(XsanInterceptor &&) = delete;
  ~XsanInterceptor() {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    recoverRealFunc();
  }

  /// Return whether the current thread is re-entering this interceptor while an
  /// outer intercepted call is still active.
  bool isRecursive() const {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    return CallDepth > 0;
  }

private:
  void recoverRealFunc() {
    if (!Modified)
      return;
    Patch.applyBackup(this->GetRealFunc());
    Modified = false;
  }

  void injectTrampoline() {
    if (Modified)
      return;
    Patch.applyPatch(this->GetRealFunc());
    Modified = true;
  }

  void InitPatch() {
    if (XsanPatch::isPatched(this->GetRealFunc())) {
      FATAL("Function %p is already patched.", this->GetMyFunc());
    }
    Patch.initialize(this->GetMyFunc(), this->GetRealFunc());
    injectTrampoline();
  }

  void PreCall() {
    Mutex.lock();
    bool Recursive = CallDepth > 0;
    ++CallDepth;
    // Recursive entries reuse the outer call's unlocked state. The outermost
    // call already holds the recursive mutex and has restored the real function
    // body, so recursive calls must not patch/unpatch again.
    if (Recursive) {
      assert(!Modified && "When recursive, the interceptee must be recovered");
      return;
    }

    recoverRealFunc();
  }

  void PostCall() {
    assert(CallDepth > 0 && "interceptor call depth underflow");
    bool Recursive = CallDepth > 1;
    --CallDepth;
    if (!Recursive)
      injectTrampoline();
    Mutex.unlock();
  }

  llvm::ArrayRef<const char *> target_procs;
  XsanPatch Patch;
  // Support recursive lock.
  mutable llvm::sys::SmartMutex<true> Mutex;
  unsigned CallDepth = 0;
  bool Modified = false;
};

} // namespace __xsan
