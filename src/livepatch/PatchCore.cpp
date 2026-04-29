#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <filesystem>
#include <limits.h>
#include <link.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/Process.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#if LLVM_VERSION_MAJOR >= 22
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#include "PatchCore.h"
#include "config_compile.h"
#include "debug.h"
#include "xsan_common.h"
#include "xsan_llvm_compat.h"

namespace fs = std::filesystem;

namespace __xsan {

namespace detail {
namespace {

class ManualInterceptionRegistry {
public:
  void registerInterceptor(llvm::StringRef Group,
                           ManualInterceptionController *Interceptor) {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    Groups[Group].Interceptors.push_back(Interceptor);
  }

  llvm::ArrayRef<ManualInterceptionController *> get(llvm::StringRef Group) {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    auto It = Groups.find(Group);
    assert(It != Groups.end() && "unknown interception group");
    return It->second.Interceptors;
  }

private:
  struct GroupState {
    llvm::SmallVector<ManualInterceptionController *, 8> Interceptors;
  };

  llvm::sys::SmartMutex<true> Mutex;
  llvm::StringMap<GroupState> Groups;
};

ManualInterceptionRegistry &getRegistry() {
  static ManualInterceptionRegistry Registry;
  return Registry;
}

class ActiveInterceptionRegistry {
public:
  void registerInterception(void *IntercepteeAddr, llvm::StringRef MangledName,
                            SourceLocation Location, const void *Owner) {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    for (const Entry &Existing : Entries) {
      if (Existing.IntercepteeAddr != IntercepteeAddr)
        continue;
      if (Existing.Owner == Owner)
        return;

      // A second livepatch for the same dynamic symbol makes symbol lookup
      // ambiguous: RTLD_DEFAULT/RTLD_NEXT can no longer be described by the
      // simple two-case relationship between the sole interceptor and the real
      // interceptee.  Fail fast and print both construction sites so the owner
      // can remove the duplicate instead of debugging a mis-patched process.
      FATAL("Attempted to intercept the same symbol twice.\n"
            "  Interceptee address: %p\n"
            "  Mangled name: %s\n"
            "  First interception: %s:%u (%s)\n"
            "  Second interception: %s:%u (%s)",
            IntercepteeAddr, Existing.MangledName.c_str(),
            Existing.Location.File, Existing.Location.Line,
            Existing.Location.Function, Location.File, Location.Line,
            Location.Function);
    }
    Entries.push_back({IntercepteeAddr, MangledName.str(), Location, Owner});
  }

  void unregisterInterception(void *IntercepteeAddr, const void *Owner) {
    llvm::sys::SmartScopedLock<true> Guard(Mutex);
    for (auto It = Entries.begin(), End = Entries.end(); It != End; ++It) {
      if (It->IntercepteeAddr == IntercepteeAddr && It->Owner == Owner) {
        Entries.erase(It);
        return;
      }
    }
  }

private:
  struct Entry {
    void *IntercepteeAddr = nullptr;
    std::string MangledName;
    SourceLocation Location;
    const void *Owner = nullptr;
  };

  llvm::sys::SmartMutex<true> Mutex;
  std::vector<Entry> Entries;
};

ActiveInterceptionRegistry &getActiveInterceptionRegistry() {
  // Intentionally leaked to avoid static-destruction-order problems: global
  // Interception objects may unregister while the process is tearing down.
  static auto *Registry = new ActiveInterceptionRegistry();
  return *Registry;
}

} // namespace

void registerManualInterception(llvm::StringRef Group,
                                ManualInterceptionController *Interceptor) {
  getRegistry().registerInterceptor(Group, Interceptor);
}

llvm::ArrayRef<ManualInterceptionController *>
getManualInterceptionGroup(llvm::StringRef Group) {
  return getRegistry().get(Group);
}

void registerActiveInterception(void *IntercepteeAddr,
                                llvm::StringRef MangledName,
                                SourceLocation Location, const void *Owner) {
  getActiveInterceptionRegistry().registerInterception(
      IntercepteeAddr, MangledName, Location, Owner);
}

void unregisterActiveInterception(void *IntercepteeAddr, const void *Owner) {
  getActiveInterceptionRegistry().unregisterInterception(IntercepteeAddr,
                                                         Owner);
}

} // namespace detail

InterceptionScope::InterceptionScope(llvm::StringRef Group)
    : InterceptionScope(detail::getManualInterceptionGroup(Group)) {}

// Lazy initialization for XsanEnabled
bool isXsanEnabled() {
  static const bool enabled = !!llvm::sys::Process::GetEnv("XSAN_COMPILE_MASK");
  return enabled;
}

// Lazy initialization for str_mask
const std::string &getStrMask() {
  static const std::string val =
      isXsanEnabled() ? *llvm::sys::Process::GetEnv("XSAN_COMPILE_MASK") : "";
  return val;
}

// Lazy initialization for xsan_mask
const std::bitset<NumSanitizerTypes> &getXsanMask() {
  static const std::bitset<NumSanitizerTypes> mask(
      isXsanEnabled() ? std::strtoul(getStrMask().c_str(), nullptr, 0) : 0);
  return mask;
}

SanitizerType getSanType() {
  static const SanitizerType sanTy = []() {
    for (SanitizerType i : {XSan, ASan, TSan, MSan, UBSan}) {
      if (getXsanMask().test(i)) {
        return i;
      }
    }
    return SanNone;
  }();
  return sanTy;
}

// r--p
// ARM64 stores rodata in executable segments (r-x), unlike x86_64 (r--)
static bool is_rodata(Elf64_Word flags) {
#if defined(__x86_64__)
  return (flags & PF_R) && !(flags & PF_W) && !(flags & PF_X);
#elif defined(__aarch64__) || defined(__arm64__)
  return (flags & PF_R) && !(flags & PF_W);
#else
#error "Unsupported architecture"
#endif
}

const std::vector<ROSegment> &getSelfModuleROSegments() {
  static std::vector<ROSegment> ro_sec;
  if (!ro_sec.empty())
    return ro_sec;
  dl_iterate_phdr(
      +[](struct dl_phdr_info *info, size_t, void *data) {
        if (info->dlpi_name != nullptr && strlen(info->dlpi_name) != 0) {
          return 0;
        }
        Elf64_Addr base = info->dlpi_addr;
        auto &ro_sec = *static_cast<std::vector<ROSegment> *>(data);
        // Main executable object
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const auto &ph = info->dlpi_phdr[i];
          if (ph.p_type == PT_LOAD && is_rodata(ph.p_flags)) {
            ro_sec.push_back({reinterpret_cast<char *>(base + ph.p_vaddr),
                              (size_t)ph.p_memsz});
          }
        }
        return 1; // 停止迭代
      },
      &ro_sec);
  return ro_sec;
}

fs::path getSelfPath() {
  static std::optional<fs::path> self_path = std::nullopt;
  if (self_path.has_value()) {
    return self_path.value();
  }

  // Get the executable path of the current process.
  char proc_path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
  if (len == -1) {
    FATAL("Failed to read /proc/self/exe");
  }
  proc_path[len] = '\0';

  // Obtain the executable name.
  self_path = fs::path(proc_path);
  return self_path.value();
}

bool isPatchingProc(const char *proc_name) {
  // Obtain the executable name.
  std::string pname = getSelfPath().filename().string();
  llvm::StringRef proc_name_str = pname;

  // Retrieve the position of proc_name in the string.
  size_t pos = pname.find(proc_name);
  if (pos == std::string::npos) {
    return false;
  }
  // Check if there is a letter character before proc_name.
  if (pos > 0 && std::isalpha(pname[pos - 1])) {
    return false;
  }
  // Check if there is a letter character after proc_name.
  size_t after_pos = pos + strlen(proc_name);
  if (after_pos < pname.length() && std::isalpha(pname[after_pos])) {
    return false;
  }
  return true;
}

fs::path getThisPatchDsoPath() {
  static std::optional<fs::path> this_dso_path = std::nullopt;
  if (this_dso_path.has_value()) {
    return this_dso_path.value();
  }
  fs::path path;
  Dl_info info;
  // Pass the address of the current function to dladdr
  if (dladdr((void *)&getThisPatchDsoPath, &info) != 0 && info.dli_fname) {
    /// Canonicalize the path to remove any symbolic links.
    this_dso_path = fs::canonical(info.dli_fname);
  } else {
    this_dso_path = fs::path();
  }
  return this_dso_path.value();
}

static fs::path canonicalizeDsoPath(const fs::path &Path) {
  if (Path.empty())
    return fs::path();

  std::error_code EC;
  fs::path Canonical = fs::weakly_canonical(Path, EC);
  return EC ? Path : Canonical;
}

static fs::path getDsoPathFromAddr(void *SymAddr) {
  Dl_info info;
  if (dladdr(SymAddr, &info) != 0 && info.dli_fname)
    return canonicalizeDsoPath(info.dli_fname);
  return fs::path();
}

fs::path getXsanAbsPath(std::string_view rel_path) {
  /// TODO: do not use env var to transmit base dir, but parse the DSO link
  /// path.
  static const auto &XsanBaseDir = llvm::sys::Process::GetEnv("XSAN_BASE_DIR");
  // <base_dir>/patch/libclang-patch.so -> <base_dir>/
  static const fs::path PatchBaseDir =
      getThisPatchDsoPath().parent_path().parent_path();
  static const bool ExistEnvVarBaseDir =
      !!XsanBaseDir && fs::exists(*XsanBaseDir);
  static const fs::path EnvVarBaseDir =
      ExistEnvVarBaseDir ? fs::canonical(*XsanBaseDir) : fs::path();

  fs::path abs_path;
  if (!XsanBaseDir) {
    abs_path = PatchBaseDir / rel_path;
  } else if (EnvVarBaseDir.empty()) {
    // xsan_base_dir has value, but EnvVarBaseDir is empty.
    WARNF("Warning: The path provided by the environment variable "
          "XSAN_BASE_DIR (\"%s\") is invalid. Using auto-detected base "
          "path: %s\n",
          XsanBaseDir->c_str(), PatchBaseDir.c_str());
    abs_path = PatchBaseDir / rel_path;
  } else {
    abs_path = EnvVarBaseDir / rel_path;
  }

  // xsan_base_dir has value, and EnvVarBaseDir is not empty.
  return abs_path;
}

fs::path getXsanArchRtDir(std::string_view triple) {
  static std::string default_triple = llvm::sys::getProcessTriple();
  static fs::path parent_dir = getXsanAbsPath(XSAN_RUNTIME_DIR "/");
  static std::optional<fs::path> fallback_triple_path =
      []() -> std::optional<fs::path> {
    if (fs::exists(parent_dir) && fs::is_directory(parent_dir)) {
      std::optional<fs::path> fallback_triple_path = std::nullopt;
      for (const auto &entry : fs::directory_iterator(parent_dir)) {
        if (fs::is_directory(entry.path())) {
          if (!fallback_triple_path.has_value())
            fallback_triple_path = entry.path();
          else
            return std::nullopt;
        }
      }
      return fallback_triple_path;
    }
    return std::nullopt;
  }();
  // If triple is empty, we should speculate the triple from the current
  // process.
  if (triple.empty())
    triple = default_triple;

  auto triple_path = getXsanAbsPath(XSAN_RUNTIME_DIR "/") / triple;
  // For some scenarios, e.g., if clang was built in another compatible system,
  // the runtime triple (e.g., x86_64-pc-linux-gnu) might not be the same as the
  // configuration-time triple, which depends on the LLVMConfig.cmake of clang
  // (e.g., x86_64-unknown-linux-gnu).
  // If there is only one triple directory, we should use it directly regardless
  // of whether the TWO triples match or not.
  if (!fs::exists(triple_path)) {
    if (fallback_triple_path.has_value()) {
      triple_path = fallback_triple_path.value();
    } else {
      FATAL("Cannot find triple directory for %s in library directory %s",
            std::string(triple).c_str(), parent_dir.c_str());
    }
  }

  return triple_path / getXsanCombName(getXsanMask());
}

std::string getXsanCombName(const std::bitset<NumSanitizerTypes> &xsan_mask) {
  if (getSanType() == XSan) {
    std::string xsan_comb_name = "xsan";
    if (xsan_mask.test(ASan))
      xsan_comb_name += "_asan";
    if (xsan_mask.test(MSan))
      xsan_comb_name += "_msan";
    if (xsan_mask.test(TSan))
      xsan_comb_name += "_tsan";
    return xsan_comb_name;
  } else {
    return "";
  }
}

const char *getMangledName(void *SymAddr) {
  Dl_info Info;
  if (!dladdr(SymAddr, &Info)) {
    FATAL("dladdr failed for address %p. Error: %s", SymAddr,
          dlerror() ? dlerror() : "Not available");
  }
  return Info.dli_sname;
}

static const char *getDSOName(void *SymAddr) {
  Dl_info Info;
  if (!dladdr(SymAddr, &Info)) {
    FATAL("dladdr failed for address %p. Error: %s", SymAddr,
          dlerror() ? dlerror() : "Not available");
  }
  return Info.dli_fname;
}

namespace {

struct LivepatchModuleMatcher {
  fs::path AnchorPath;
  std::string AnchorFilename;

  explicit LivepatchModuleMatcher(void *DsoAnchor) {
    AnchorPath =
        DsoAnchor ? getDsoPathFromAddr(DsoAnchor) : getThisPatchDsoPath();
    AnchorPath = canonicalizeDsoPath(AnchorPath);
    AnchorFilename = AnchorPath.filename().string();
  }

  bool isLivepatchPath(const fs::path &Path) const {
    fs::path CanonicalPath = canonicalizeDsoPath(Path);
    if (CanonicalPath.empty())
      return false;
    if (!AnchorPath.empty() && CanonicalPath == AnchorPath)
      return true;
    return !AnchorFilename.empty() &&
           CanonicalPath.filename() == AnchorFilename;
  }

  bool isLivepatchAddr(void *Addr) const {
    return isLivepatchPath(getDsoPathFromAddr(Addr));
  }
};

struct DynamicSymbolLookup {
  const char *MangledName = nullptr;
  const LivepatchModuleMatcher *Matcher = nullptr;
  void *Addr = nullptr;
};

static int lookupDynamicSymbolInLoadedObject(dl_phdr_info *Info, size_t,
                                             void *Data) {
  auto &Lookup = *static_cast<DynamicSymbolLookup *>(Data);

  const bool IsMainExecutable = !Info->dlpi_name || !Info->dlpi_name[0];
  fs::path ObjectPath =
      IsMainExecutable ? getSelfPath() : canonicalizeDsoPath(Info->dlpi_name);
  if (Lookup.Matcher->isLivepatchPath(ObjectPath))
    return 0;

  void *Handle = nullptr;
  if (IsMainExecutable) {
    Handle = dlopen(nullptr, RTLD_LAZY);
  } else {
    Handle = dlopen(ObjectPath.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  }
  if (!Handle)
    return 0;

  dlerror();
  void *Addr = dlsym(Handle, Lookup.MangledName);
  const char *Err = dlerror();
  if (!IsMainExecutable)
    dlclose(Handle);

  if (!Addr || Err || Lookup.Matcher->isLivepatchAddr(Addr))
    return 0;

  Lookup.Addr = Addr;
  return 1;
}

static void *
findNonLivepatchDynamicSymbol(const char *MangledName,
                              const LivepatchModuleMatcher &Matcher) {
  DynamicSymbolLookup Lookup{MangledName, &Matcher, nullptr};
  dl_iterate_phdr(lookupDynamicSymbolInLoadedObject, &Lookup);
  return Lookup.Addr;
}

} // namespace

// Find the real interceptee symbol, not the livepatch replacement.
//
// Prefer the loader's normal global/next lookups for the two common dynamic
// relationships:
//   1. interceptee before interceptor: RTLD_DEFAULT may see the real symbol;
//   2. interceptor before interceptee: RTLD_NEXT may see the real symbol.
//
// Some toolchains expose the interceptee from the executable dynsym, but
// RTLD_DEFAULT still resolves to the preloaded livepatch interceptor and
// RTLD_NEXT cannot walk "back" to the executable.  For that case, scan loaded
// objects and query each object handle with dlsym, still limiting support to
// dlsym-visible dynamic symbols.
//
// We intentionally rely only on dlsym-visible dynamic symbols here.  Executable
// dynsym entries are covered by RTLD_DEFAULT; non-dynsym executable symbols are
// future work and should be implemented with the shared ELF utilities, not
// custom parsing in this path.
void *getIntercepteeAddr(const char *MangledName, void *DsoAnchor) {
  LivepatchModuleMatcher Matcher(DsoAnchor);

  dlerror();
  void *Addr = dlsym(RTLD_DEFAULT, MangledName);
  const char *Err = dlerror();
  if (Addr && !Err && !Matcher.isLivepatchAddr(Addr))
    return Addr;

  if (Addr && !Err) {
    dlerror();
    void *NextAddr = dlsym(RTLD_NEXT, MangledName);
    const char *NextErr = dlerror();
    if (NextAddr && !NextErr && !Matcher.isLivepatchAddr(NextAddr))
      return NextAddr;

    if (void *ObjectAddr = findNonLivepatchDynamicSymbol(MangledName, Matcher))
      return ObjectAddr;

    FATAL("Cannot find the real interceptee addr for symbol %s after "
          "RTLD_DEFAULT found the livepatch interceptor at %p. RTLD_NEXT "
          "returned %p%s%s",
          MangledName, Addr, NextAddr, NextErr ? ": " : "",
          NextErr ? NextErr : "");
  }

  if (void *ObjectAddr = findNonLivepatchDynamicSymbol(MangledName, Matcher))
    return ObjectAddr;

  FATAL("Cannot find the real interceptee addr for symbol %s via "
        "RTLD_DEFAULT: %s",
        MangledName, Err ? Err : "not found");
}

void *getIntercepteeAddr(void *InterceptorFunc) {
  const char *MangledName = getMangledName(InterceptorFunc);
  return getIntercepteeAddr(MangledName, InterceptorFunc);
}

void *getIntercepteeAddr(void *SymbolFunc, void *DsoAnchor) {
  const char *MangledName = getMangledName(SymbolFunc);
  return getIntercepteeAddr(MangledName, DsoAnchor);
}

// Find the symbol in the patch DSO.
// 1. Search it by RTLD_DEFAULT first.
// 2. If not found, try to find it via explicit dlopen.
void *getInterceptorAddr(const char *MangledName, void *DsoAnchor) {
  void *addr = nullptr;
  fs::path targetDso =
      DsoAnchor ? getDsoPathFromAddr(DsoAnchor) : getThisPatchDsoPath();
  if (targetDso.empty())
    FATAL("getInterceptorAddr: failed to resolve target DSO for %s",
          MangledName);

  dlerror(); // clear previous error
  addr = dlsym(RTLD_DEFAULT, MangledName);
  const char *err = dlerror();
  if (!addr || err) {
    FATAL("getInterceptorAddr: dlsym(RTLD_DEFAULT, %s) failed: %s", MangledName,
          err ? err : "null");
  }
  const char *DSOName = getDSOName(addr);
  std::string patchName = targetDso.filename().string();
  // If the symbol is in the patch DSO, return it.
  if (compat::endsWith(llvm::StringRef(DSOName), patchName))
    return addr;
  // If the symbol is not in the patch DSO, it might be masked by the main
  // executable. Try to find it via explicit dlopen.
  // RTLD_NOLOAD is used to search the symbol in the loaded DSOs.
  void *handle = dlopen(targetDso.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (!handle) {
    FATAL("getInterceptorAddr: dlopen(%s) failed: %s", targetDso.c_str(),
          dlerror());
  }
  dlerror();
  addr = dlsym(handle, MangledName);
  err = dlerror();
  if (!addr || err) {
    FATAL("getInterceptorAddr: dlsym(patch_so, %s) failed: %s", MangledName,
          err ? err : "null");
  }
  const char *my_dso = getDSOName(addr);
  if (!compat::endsWith(llvm::StringRef(my_dso), patchName))
    FATAL("getInterceptorAddr: found symbol %s is not in patch so", my_dso);
  // Do not dlclose(handle), because RTLD_NOLOAD does not increase the reference
  // count.
  return addr;
}

class ScopedApplyPatch {
  using Memory = llvm::sys::Memory;

public:
  enum class MemoryType {
    Exec,
    RoData,
  };

private:
  void changeMemoryProtection(unsigned Flags) {
    if (auto Ret = Memory::protectMappedMemory(Mem, Flags)) {
      FATAL("Failed to protect memory for patching: %s", Ret.message().c_str());
    }
  }

public:
  unsigned getMemoryProtectionFlags(MemoryType Type) {
    switch (Type) {
    case MemoryType::Exec:
      return Memory::MF_EXEC | Memory::MF_READ;
    case MemoryType::RoData:
      return Memory::MF_READ;
    }
  }

  ScopedApplyPatch(void *FuncAddr, size_t n, MemoryType Type = MemoryType::Exec)
      : Mem(FuncAddr, n), Type(Type) {
    changeMemoryProtection(Memory::MF_RWE_MASK);
  }
  ~ScopedApplyPatch() {
    changeMemoryProtection(getMemoryProtectionFlags(Type));
  }

private:
  llvm::sys::MemoryBlock Mem;
  MemoryType Type;
};

// Memcpy, but can run on address without write permission.
void memcpy_forcibly(void *dst, const void *src, size_t n, bool is_dst_exec) {
  ScopedApplyPatch::MemoryType MemTy =
      is_dst_exec ? ScopedApplyPatch::MemoryType::Exec
                  : ScopedApplyPatch::MemoryType::RoData;
  ScopedApplyPatch Scoper(dst, n, MemTy);
  std::memcpy(dst, src, n);
}

void XsanPatch::validateInit() const {
  if (!isInitialized)
    FATAL("Cannot use patch without initialization");
}

void XsanPatch::initialize(void *Interceptor, void *Interceptee) {
  std::memcpy(patch, PatchTempl, PatchSize);
  std::memcpy(&patch[FuncAddrOffset], (void *)&Interceptor, sizeof(void *));
  std::memcpy(backup, Interceptee, PatchSize);
  isInitialized = true;
}

bool XsanPatch::isPatched(void *FuncAddr) {
  uint8_t *addr = (uint8_t *)FuncAddr;
  return std::equal(PatchTempl, PatchTempl + FuncAddrOffset, addr) &&
         std::equal(PatchTempl + FuncAddrOffset + sizeof(void *),
                    PatchTempl + PatchSize,
                    addr + FuncAddrOffset + sizeof(void *));
}

void XsanPatch::applyPatch(void *FuncAddr) const {
  validateInit();
  memcpy_forcibly(FuncAddr, patch, sizeof(Data), true);
}

void XsanPatch::applyBackup(void *FuncAddr) const {
  validateInit();
  memcpy_forcibly(FuncAddr, backup, sizeof(Data), true);
}

// Return the base address of the executable file
void *get_base_address() {
  static void *base_addr = []() {
    std::filesystem::path exe_path = getSelfPath();
    std::string exe_path_str = exe_path.string();
    struct ctx_t {
      std::string_view exe_path_str;
      void *base_addr;
    } ctx = {exe_path_str, (void *)1};
    dl_iterate_phdr(
        [](struct dl_phdr_info *info, size_t size, void *data) -> int {
          auto *ctx = static_cast<ctx_t *>(data);
          if (!info->dlpi_name || info->dlpi_name[0] == '\0' ||
              ctx->exe_path_str == info->dlpi_name) {
            ctx->base_addr = (void *)info->dlpi_addr;
            return 1; // Stop iteration
          }
          return 0; // Continue iteration
        },
        &ctx);
    return ctx.base_addr;
  }();
  if (base_addr == (void *)1)
    FATAL("Failed to find base address via dl_iterate_phdr");
  return base_addr;
}

bool is_self_proc_pie() { return get_base_address() != (void *)0; }
} // namespace __xsan
