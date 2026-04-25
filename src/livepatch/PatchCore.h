#pragma once

// Match functions

#include "debug.h"
#include "xsan_common.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Mutex.h"
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace __xsan {

template <typename FunPtr = void *,
          typename = std::enable_if_t<
              std::is_function_v<std::remove_pointer_t<FunPtr>> ||
              std::is_member_function_pointer_v<FunPtr> ||
              std::is_same_v<FunPtr, void *>>>
class ManualInterception;

class ManualInterceptionController {
public:
  virtual ~ManualInterceptionController() = default;
  virtual void enableInterception() = 0;
  virtual void disableInterception() = 0;
};

namespace detail {

void registerManualInterception(llvm::StringRef Group,
                                ManualInterceptionController *Interceptor);
llvm::ArrayRef<ManualInterceptionController *>
getManualInterceptionGroup(llvm::StringRef Group);

template <typename FunTy1, typename FunTy2>
struct IsAbiCompatibleFunTyPair
    : std::is_same<std::remove_cv_t<FunTy1>, std::remove_cv_t<FunTy2>> {};

template <typename Ret, typename Class, typename... Args>
struct IsAbiCompatibleFunTyPair<Ret (*)(Class *, Args...),
                                Ret (Class::*)(Args...)> : std::true_type {};

// Ret (This)(Arg) <-> Ret (This, Arg)
template <typename Ret, typename Class, typename... Args>
struct IsAbiCompatibleFunTyPair<Ret (*)(const Class *, Args...),
                                Ret (Class::*)(Args...) const>
    : std::true_type {};

template <typename FunTy1, typename FunTy2>
inline constexpr bool IsAbiCompatibleFunTyPairV =
    IsAbiCompatibleFunTyPair<FunTy1, FunTy2>::value;

} // namespace detail

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
void *getIntercepteeAddr(const char *MangledName, void *DsoAnchor = nullptr);
void *getIntercepteeAddr(void *InterceptorFunc);
// Resolve a real function by the mangled name of another symbol. This is used
// when the interceptor body and interceptee have the same type but different
// symbol addresses; SymbolFunc supplies the mangled interceptee name, DsoAnchor
// identifies the patch DSO to exclude from RTLD_DEFAULT fallback.
void *getIntercepteeAddr(void *SymbolFunc, void *DsoAnchor);

// Find the symbol in the patch DSO.
// 1. Search it by RTLD_DEFAULT first.
// 2. If not found, try to find it via explicit dlopen.
void *getInterceptorAddr(const char *MangledName, void *DsoAnchor = nullptr);

// Replace the first n instructions to trampoline instructions
class XsanPatch {
#if defined(__x86_64__)
  static constexpr uint8_t PatchTempl[] = {
      // movabs rax, <Interceptor>
      0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
      // jmp rax
      0xff, 0xe0};
  static constexpr size_t FuncAddrOffset = 2; // Offset for Interceptor address
#elif defined(__aarch64__)
  static constexpr uint8_t PatchTempl[] = {
      // ldr x16, 0x8
      0x50, 0x00, 0x00, 0x58,
      // br x16
      0x00, 0x02, 0x1f, 0xd6,
      // .quad <Interceptor>
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr size_t FuncAddrOffset = 8; // Offset for Interceptor address
#elif defined(__loongarch__) && __loongarch_grlen == 64
  static constexpr uint8_t PatchTempl[] = {
      // pcaddu18i $t0, 0
      0x0c, 0x00, 0x00, 0x1e,
      // ld.d $t0, $t0, 12
      0x8c, 0x31, 0xc0, 0x28,
      // jr $t0
      0x80, 0x01, 0x00, 0x4c,
      // .quad <Interceptor>
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr size_t FuncAddrOffset = 12; // Offset for Interceptor address
#elif defined(__riscv) && __riscv_xlen == 64
  static constexpr uint8_t PatchTempl[] = {
      // auipc t0, 0x0
      0x97, 0x02, 0x00, 0x00,
      // ld t0, 10(t0)
      0x83, 0xb2, 0xa2, 0x00,
      // c.jr t0
      0x82, 0x82,
      // .quad <Interceptor>
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr size_t FuncAddrOffset = 10; // Offset for Interceptor address
#else
#error "Unsupported architecture"
#endif
  static constexpr size_t PatchSize = sizeof(PatchTempl);
  using Data = uint8_t[PatchSize];

public:
  XsanPatch() = default;
  XsanPatch(void *Interceptor, void *Interceptee) {
    initialize(Interceptor, Interceptee);
  }

  static bool isPatched(void *FuncAddr);

  void initialize(void *Interceptor, void *Interceptee);
  void applyPatch(void *FuncAddr) const;
  void applyBackup(void *FuncAddr) const;

private:
  void validateInit() const;

private:
  bool isInitialized;
  Data patch;
  Data backup;
};

template <typename DeriveTy, typename FunPtr> class InterceptionFunctorBase {
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
  void *GetInterceptor() const { return get(Interceptor); }
  void *GetInterceptee() const { return get(Interceptee); }
  void SetInterceptor(void *p) { set(p, Interceptor); }
  void SetInterceptee(void *p) { set(p, Interceptee); }

  void PreCall() {}
  void PostCall() {}

  class ScopedCall {
  public:
    ScopedCall(InterceptionFunctorBase *self)
        : Self(static_cast<DeriveTy *>(self)) {
      Self->PreCall();
    }
    ~ScopedCall() { Self->PostCall(); }

  private:
    DeriveTy *Self;
  };

  FunPtr Interceptor{};
  FunPtr Interceptee{};
};

template <typename DeriveTy, typename FunPtr>
class InterceptionFunctor
    : public InterceptionFunctorBase<DeriveTy, void *> {
protected:
  using ScopedCall =
      typename InterceptionFunctorBase<DeriveTy, void *>::ScopedCall;
};

// Common function pointers (e.g., Ret (*)(Args...))
template <typename DeriveTy, typename Ret, typename... Args>
class InterceptionFunctor<DeriveTy, Ret (*)(Args...)>
    : public InterceptionFunctorBase<DeriveTy, Ret (*)(Args...)> {
protected:
  using ScopedCall =
      typename InterceptionFunctorBase<DeriveTy,
                                          Ret (*)(Args...)>::ScopedCall;

public:
  Ret operator()(Args... args) {
    ScopedCall Scoper(this);
    return (this->Interceptee)(std::forward<Args>(args)...);
  }
};

// Variadic function pointers (e.g., Ret (*)(Args..., ...))
template <typename DeriveTy, typename Ret, typename... Args>
class InterceptionFunctor<DeriveTy, Ret (*)(Args..., ...)>
    : public InterceptionFunctorBase<DeriveTy, Ret (*)(Args..., ...)> {
protected:
  using ScopedCall =
      typename InterceptionFunctorBase<DeriveTy,
                                          Ret (*)(Args..., ...)>::ScopedCall;

public:
  template <typename... CallArgs> Ret operator()(CallArgs &&...args) {
    // Note: we can't do parameter number check here, must ensure that the
    // caller passes the correct parameters to the real function.
    ScopedCall Scoper(this);
    return (this->Interceptee)(std::forward<CallArgs>(args)...);
  }
};

template <typename DeriveTy, typename Ret, typename Class, typename... Args>
class InterceptionFunctor<DeriveTy, Ret (Class::*)(Args...)>
    : public InterceptionFunctorBase<DeriveTy, Ret (Class::*)(Args...)> {
protected:
  using ScopedCall =
      typename InterceptionFunctorBase<DeriveTy,
                                          Ret (Class::*)(Args...)>::ScopedCall;

public:
  Ret operator()(Class *obj, Args... args) {
    ScopedCall Scoper(this);
    return (obj->*(this->Interceptee))(std::forward<Args>(args)...);
  }
};

/// Always-on livepatch plus callable handle to the original interceptee.
///
/// Terms:
/// - Interceptor: replacement function in the patch DSO; patched calls jump to
///   it.
/// - Interceptee: original function being patched.
/// - Interception object: call it inside the interceptor body to call the
///   original interceptee. For member functions, pass the object as arg 0.
/// - target_procs: executable-name allowlist. No match => this object is inert.
///
/// Quick use:
///   static Interception I(&foo, {"clang"});            // same-name hook
///   static Interception I(&hook, "_Z3foov", {"clang"}); // explicit original
///   static Interception I(&hook, &foo, {"clang"});     // typed original
///   static Interception I("_Z3foov", {"clang"});       // find hook by name
///
/// Interception object as the interceptee call handle:
///   Ret foo(Args... A) { ...; return I(A...); }        // calls original foo
///
/// Constructor forms:
/// - Interception(Interceptor, target_procs)
///   Same-name form. Use Interceptor's mangled name for both symbols.
/// - Interception(Interceptor, IntercepteeMangledName, target_procs)
///   Use Interceptor as the replacement; find the original by mangled name.
/// - Interception(Interceptor, Interceptee, target_procs)
///   Use Interceptor as the replacement; use Interceptee only to derive the
///   original symbol name and this object's callable type. If Interceptee is a
///   member function, Interceptor may be the ABI-equivalent free function whose
///   first argument is the object pointer.
/// - Interception(MangledName, target_procs[, DsoAnchor])
///   Same-name form when the interceptor address is not passed directly; find
///   the interceptor in the patch DSO and the original outside it.
/// - DsoAnchor identifies the patch DSO when name lookup needs that boundary.
///
/// While the Interception object is being called, the patch is temporarily
/// removed so that the call reaches the original interceptee. Recursive calls
/// share that restored state.
///
/// Non-effective/fatal cases:
/// - If target_procs does not match the executable name, no patch is installed.
/// - While any thread is calling the original via this object, the interceptee
///   patch is temporarily removed process-wide; concurrent direct/non-PLT calls
///   to that interceptee are not intercepted during that window.
/// - Same-name forms require interceptor/interceptee to have the same mangled
///   name; otherwise lookup fails or patches the wrong function.
/// - Hidden/unloaded/missing symbols and already-patched functions are fatal.
template <typename FunPtr = void *,
          typename = std::enable_if_t<
              std::is_function_v<std::remove_pointer_t<FunPtr>> ||
              std::is_member_function_pointer_v<FunPtr> ||
              std::is_same_v<FunPtr, void *>>>
class Interception
    : public InterceptionFunctor<Interception<FunPtr>,
                                    std::remove_cv_t<FunPtr>> {
  using BaseTy =
      InterceptionFunctor<Interception<FunPtr>, std::remove_cv_t<FunPtr>>;
  friend BaseTy;
  friend typename BaseTy::ScopedCall;

protected:
  enum class InitialInterceptionState {
    Enabled,
    Disabled,
  };

  template <typename PtrTy> static void *getFunctionAddress(PtrTy Fn) {
    void *Addr;
    if constexpr (sizeof(PtrTy) == sizeof(void *)) {
      Addr = reinterpret_cast<void *>(Fn);
    } else {
      std::memcpy(&Addr, &Fn, sizeof(void *));
    }
    return Addr;
  }

  template <typename InterceptorFunPtr>
  static void *getCheckedInterceptorAddress(InterceptorFunPtr Interceptor) {
    static_assert(
        detail::IsAbiCompatibleFunTyPairV<InterceptorFunPtr, FunPtr>,
        "Interception requires the interceptor to have the same type as the "
        "interceptee, or to be ABI-compatible with a member-function "
        "interceptee by taking the object pointer as the first parameter");
    return getFunctionAddress(Interceptor);
  }

public:
  Interception(FunPtr Interceptor, llvm::ArrayRef<const char *> target_procs)
      : Interception(getFunctionAddress(Interceptor), nullptr, nullptr,
                     target_procs, InitialInterceptionState::Enabled) {}
  Interception(FunPtr Interceptor, const char *IntercepteeMangledName,
               llvm::ArrayRef<const char *> target_procs)
      : Interception(getFunctionAddress(Interceptor), IntercepteeMangledName,
                     nullptr, target_procs,
                     InitialInterceptionState::Enabled) {}
  template <typename InterceptorFunPtr>
  Interception(InterceptorFunPtr Interceptor, FunPtr Interceptee,
               llvm::ArrayRef<const char *> target_procs)
      : Interception(getCheckedInterceptorAddress(Interceptor), nullptr,
                     getFunctionAddress(Interceptee), target_procs,
                     InitialInterceptionState::Enabled) {}
  Interception(const char *MangledName,
               llvm::ArrayRef<const char *> target_procs)
      : Interception(nullptr, MangledName, nullptr, target_procs,
                     InitialInterceptionState::Enabled) {}
  Interception(const char *MangledName,
               llvm::ArrayRef<const char *> target_procs, void *DsoAnchor)
      : Interception(nullptr, MangledName, nullptr, target_procs,
                     InitialInterceptionState::Enabled, DsoAnchor) {}
  Interception(const Interception &) = delete;
  Interception(Interception &&) = delete;
  Interception &operator=(const Interception &) = delete;
  Interception &operator=(Interception &&) = delete;
  ~Interception() {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    recoverInterceptee();
  }

  /// Return whether the current thread is re-entering this interceptor while an
  /// outer intercepted call is still active.
  bool isRecursive() const {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    return CallDepth > 0;
  }

protected:
  Interception(FunPtr Interceptor, llvm::ArrayRef<const char *> target_procs,
               InitialInterceptionState InitialState)
      : Interception(getFunctionAddress(Interceptor), nullptr, nullptr,
                     target_procs, InitialState) {}
  Interception(FunPtr Interceptor, const char *IntercepteeMangledName,
               llvm::ArrayRef<const char *> target_procs,
               InitialInterceptionState InitialState)
      : Interception(getFunctionAddress(Interceptor), IntercepteeMangledName,
                     nullptr, target_procs, InitialState) {}
  template <typename InterceptorFunPtr>
  Interception(InterceptorFunPtr Interceptor, FunPtr Interceptee,
               llvm::ArrayRef<const char *> target_procs,
               InitialInterceptionState InitialState)
      : Interception(getCheckedInterceptorAddress(Interceptor), nullptr,
                     getFunctionAddress(Interceptee), target_procs,
                     InitialState) {}
  Interception(const char *MangledName,
               llvm::ArrayRef<const char *> target_procs,
               InitialInterceptionState InitialState)
      : Interception(nullptr, MangledName, nullptr, target_procs,
                     InitialState) {}
  Interception(const char *MangledName,
               llvm::ArrayRef<const char *> target_procs,
               InitialInterceptionState InitialState, void *DsoAnchor)
      : Interception(nullptr, MangledName, nullptr, target_procs, InitialState,
                     DsoAnchor) {}

  // The single full constructor. Short constructors above only fill these
  // optional knobs:
  // - InterceptorAddr: patch target jumps here; if null, resolve by mangled name
  // - IntercepteeMangledName: explicit real function name
  // - IntercepteeSymbol: explicit real function symbol; wins over mangled name
  // - DsoAnchor: patch DSO identity when resolving same-name hooks
  Interception(void *InterceptorAddr, const char *IntercepteeMangledName,
               void *IntercepteeSymbol,
               llvm::ArrayRef<const char *> target_procs,
               InitialInterceptionState InitialState,
               void *DsoAnchor = nullptr)
      : target_procs(target_procs) {
    if (!llvm::any_of(target_procs, isPatchingProc))
      return;

    if (!InterceptorAddr) {
      assert(IntercepteeMangledName &&
             "mangled name is required when interceptor is not provided");
      InterceptorAddr = getInterceptorAddr(IntercepteeMangledName, DsoAnchor);
    }

    this->SetInterceptor(InterceptorAddr);
    if (IntercepteeSymbol) {
      this->SetInterceptee(
          getIntercepteeAddr(IntercepteeSymbol, this->GetInterceptor()));
    } else if (IntercepteeMangledName) {
      this->SetInterceptee(
          getIntercepteeAddr(IntercepteeMangledName, this->GetInterceptor()));
    } else {
      this->SetInterceptee(getIntercepteeAddr(this->GetInterceptor()));
    }
    InitPatch(InitialState);
  }

  void enableInterceptionPatch() {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    if (!Initialized)
      return;
    assert(CallDepth == 0 &&
           "cannot enable interception while interceptor is active");
    InterceptionEnabled = true;
    injectTrampoline();
  }

  void disableInterceptionPatch() {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    if (!Initialized)
      return;
    assert(CallDepth == 0 &&
           "cannot disable interception while interceptor is active");
    recoverInterceptee();
    InterceptionEnabled = false;
  }

  bool isInitialized() const {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    return Initialized;
  }

  void recoverInterceptee() {
    if (!Modified)
      return;
    Patch.applyBackup(this->GetInterceptee());
    Modified = false;
  }

  void injectTrampoline() {
    if (Modified)
      return;
    Patch.applyPatch(this->GetInterceptee());
    Modified = true;
  }

  void InitPatch(InitialInterceptionState InitialState) {
    if (XsanPatch::isPatched(this->GetInterceptee())) {
      FATAL("Function %p is already patched.", this->GetInterceptor());
    }
    Patch.initialize(this->GetInterceptor(), this->GetInterceptee());
    Initialized = true;
    InterceptionEnabled =
        InitialState == InitialInterceptionState::Enabled;
    if (InitialState == InitialInterceptionState::Enabled)
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

    if (!InterceptionEnabled)
      return;
    recoverInterceptee();
  }

  void PostCall() {
    assert(CallDepth > 0 && "interceptor call depth underflow");
    bool Recursive = CallDepth > 1;
    --CallDepth;
    if (!Recursive && InterceptionEnabled)
      injectTrampoline();
    Mutex.unlock();
  }

  llvm::ArrayRef<const char *> target_procs;
  XsanPatch Patch;
  // Support recursive lock.
  mutable llvm::sys::SmartMutex<true> Mutex;
  unsigned CallDepth = 0;
  bool Modified = false;
  bool Initialized = false;
  bool InterceptionEnabled = false;
};

/// Disabled-by-default livepatch plus callable handle to the original
/// interceptee.
///
/// It has the same Interceptor/Interceptee/target_procs/DsoAnchor parameters as
/// Interception, and the same rule: call the ManualInterception object inside
/// the interceptor body to call the original interceptee.
///
/// Extra parameter:
/// - Group: optional registry key. Empty Group means "do not register"; control
///   this object only with enableInterception()/disableInterception(). Non-empty
///   Group lets InterceptionScope(Group) enable/disable all registered entries.
///
/// Quick use:
///   static ManualInterception I(&foo, {"clang"});        // no registry
///   I.enableInterception(); ...; I.disableInterception();
///
///   static ManualInterception I(&hook, "_Z3foov", {"clang"});
///   static ManualInterception I(&hook, &foo, {"clang"});
///   static ManualInterception I("_Z3foov", {"clang"});
///
///   static ManualInterception I(&foo, {"clang"}, "desan"); // group registry
///   InterceptionScope Scope("desan");                       // enables group
///
/// Constructor forms:
///   (&foo, targets), (&hook, mangled, targets), (&hook, &foo, targets),
///   (mangled, targets). Add Group after target_procs to register it.
///   When &foo is passed as a typed interceptee, the ManualInterception type is
///   deduced from that interceptee. Without a typed interceptee, it is deduced
///   from the interceptor itself.
///
/// Non-effective/fatal cases are the same as Interception. Additionally, a
/// ManualInterception starts disabled: it does not intercept until manually
/// enabled or, if grouped, until a matching InterceptionScope is active.
template <typename FunPtr, typename>
class ManualInterception : public Interception<FunPtr>,
                           public ManualInterceptionController {
  using BaseTy = Interception<FunPtr>;

public:
  ManualInterception(FunPtr Interceptor,
                     llvm::ArrayRef<const char *> target_procs,
                     llvm::StringRef Group = {})
      : ManualInterception(BaseTy::getFunctionAddress(Interceptor), nullptr,
                           nullptr, target_procs, Group) {}

  template <typename InterceptorFunPtr>
  ManualInterception(InterceptorFunPtr Interceptor, FunPtr Interceptee,
                     llvm::ArrayRef<const char *> target_procs,
                     llvm::StringRef Group = {})
      : ManualInterception(
            BaseTy::template getCheckedInterceptorAddress<InterceptorFunPtr>(
                Interceptor),
            nullptr, BaseTy::getFunctionAddress(Interceptee), target_procs,
            Group) {}

  ManualInterception(FunPtr Interceptor, const char *IntercepteeMangledName,
                     llvm::ArrayRef<const char *> target_procs,
                     llvm::StringRef Group = {})
      : ManualInterception(BaseTy::getFunctionAddress(Interceptor),
                           IntercepteeMangledName, nullptr, target_procs,
                           Group) {}

  ManualInterception(const char *MangledName,
                     llvm::ArrayRef<const char *> target_procs,
                     llvm::StringRef Group = {}, void *DsoAnchor = nullptr)
      : ManualInterception(nullptr, MangledName, nullptr, target_procs, Group,
                           DsoAnchor) {}

  ManualInterception(const ManualInterception &) = delete;
  ManualInterception(ManualInterception &&) = delete;
  ManualInterception &operator=(const ManualInterception &) = delete;
  ManualInterception &operator=(ManualInterception &&) = delete;

  ~ManualInterception() {
    BaseTy::disableInterceptionPatch();
  }

  void enableInterception() override { BaseTy::enableInterceptionPatch(); }

  void disableInterception() override { BaseTy::disableInterceptionPatch(); }

private:
  ManualInterception(void *InterceptorAddr, const char *IntercepteeMangledName,
                     void *IntercepteeSymbol,
                     llvm::ArrayRef<const char *> target_procs,
                     llvm::StringRef Group = {}, void *DsoAnchor = nullptr)
      : BaseTy(InterceptorAddr, IntercepteeMangledName, IntercepteeSymbol,
               target_procs, BaseTy::InitialInterceptionState::Disabled,
               DsoAnchor),
        Group(Group.str()) {
    registerGroupIfRequested();
  }

  void registerGroupIfRequested() {
    if (Group.empty() || !BaseTy::isInitialized())
      return;
    detail::registerManualInterception(Group, this);
  }

  std::string Group;
};

/// RAII handle for a named group or an explicit ManualInterception list.
///
/// Group mode toggles all ManualInterception objects registered under Group.
/// List mode toggles only the provided objects; they do not need a Group.
///
/// Usage:
///   static ManualInterception FooInterceptor(..., "desan");
///   InterceptionScope Scope("desan");                 // group mode
///
///   static ManualInterception BarInterceptor(...);     // no registry
///   ManualInterceptionController *Interceptors[] = {&BarInterceptor};
///   InterceptionScope Scope(Interceptors);             // list mode
///
/// InterceptionScope itself does not care whether entries came from a group or
/// an explicit list: construction enables entries in order, destruction
/// disables them in reverse order.
class InterceptionScope {
public:
  explicit InterceptionScope(llvm::StringRef Group);
  explicit InterceptionScope(
      llvm::ArrayRef<ManualInterceptionController *> Interceptions)
      : Interceptions(Interceptions.begin(), Interceptions.end()) {
    assert(!this->Interceptions.empty() &&
           "explicit interception scope requires at least one interception");
    validateInterceptions();
    enableInterceptions();
  }

  ~InterceptionScope() { disableInterceptions(); }

  InterceptionScope(const InterceptionScope &) = delete;
  InterceptionScope(InterceptionScope &&) = delete;
  InterceptionScope &operator=(const InterceptionScope &) = delete;
  InterceptionScope &operator=(InterceptionScope &&) = delete;

private:
  void validateInterceptions() const {
    for (auto *Interception : Interceptions)
      assert(Interception && "explicit interception scope entry is null");
  }

  void enableInterceptions() {
    for (auto *Interception : Interceptions)
      Interception->enableInterception();
  }

  void disableInterceptions() {
    for (auto It = Interceptions.rbegin(); It != Interceptions.rend(); ++It)
      (*It)->disableInterception();
  }

  llvm::SmallVector<ManualInterceptionController *, 8> Interceptions;
};

} // namespace __xsan
