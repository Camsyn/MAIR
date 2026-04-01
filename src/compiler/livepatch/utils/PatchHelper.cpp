#include <cstdio>
#include <dlfcn.h>
#include <elf.h>
#include <filesystem>
#include <limits.h>
#include <link.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/Process.h>
#include <optional>
#include <string_view>

#if LLVM_VERSION_MAJOR >= 22
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#include "PatchHelper.h"
#include "config_compile.h"
#include "debug.h"
#include "types.h"
#include "xsan_common.h"
#include "xsan_llvm_compat.h"

namespace fs = std::filesystem;

namespace __xsan {

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

static const char *getMangledName(void *SymAddr) {
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

// Find the symbol in the main executable or other DSOs.
// 1. Search it by RTLD_NEXT first.
// 2. If not found, try to find it via RTLD_DEFAULT.
void *getRealFuncAddr(const char *ManagledName) {
  void *addr = nullptr;
  dlerror(); // clear previous error
  addr = dlsym(RTLD_NEXT, ManagledName);
  const char *err = dlerror();
  // If RTLD_NEXT found the symbol, return it.
  if (addr && !err) {
    return addr;
  }
  // RTLD_NEXT not found, try RTLD_DEFAULT (e.g., symbol might exist in the
  // executable)
  addr = dlsym(RTLD_DEFAULT, ManagledName);
  err = dlerror();
  if (!addr || err) {
    FATAL("getRealFuncAddr: cannot find symbol %s, RTLD_NEXT/RTLD_DEFAULT "
          "both failed: %s",
          ManagledName, err ? err : "null");
  }
  // If RTLD_DEFAULT return the symbol of patch so, it means the symbol cannot
  // be found either in the executable or other DSOs.
  const char *DSOName = getDSOName(addr);
  auto selfPatchDso = getThisPatchDsoPath();
  std::string patchName = selfPatchDso.filename().string();
  if (llvm::StringRef(DSOName).endswith(patchName)) {
    FATAL("Cannot find the real addr for dynamic symbol %s", ManagledName);
  }
  return addr;
}

void *getRealFuncAddr(void *InterceptorFunc) {
  const char *ManagledName = getMangledName(InterceptorFunc);
  return getRealFuncAddr(ManagledName);
}

// Find the symbol in the patch DSO.
// 1. Search it by RTLD_DEFAULT first.
// 2. If not found, try to find it via explicit dlopen.
void *getMyFuncAddr(const char *ManagledName) {
  void *addr = nullptr;
  dlerror(); // clear previous error
  addr = dlsym(RTLD_DEFAULT, ManagledName);
  const char *err = dlerror();
  if (!addr || err) {
    FATAL("getMyFuncAddr: dlsym(RTLD_DEFAULT, %s) failed: %s", ManagledName,
          err ? err : "null");
  }
  const char *DSOName = getDSOName(addr);
  auto selfPatchDso = getThisPatchDsoPath();
  std::string patchName = selfPatchDso.filename().string();
  // If the symbol is in the patch DSO, return it.
  if (llvm::StringRef(DSOName).endswith(patchName)) {
    return addr;
  }
  // If the symbol is not in the patch DSO, it might be masked by the main
  // executable. Try to find it via explicit dlopen.
  // RTLD_NOLOAD is used to search the symbol in the loaded DSOs.
  void *handle = dlopen(selfPatchDso.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (!handle) {
    FATAL("getMyFuncAddr: dlopen(%s) failed: %s", selfPatchDso.c_str(),
          dlerror());
  }
  dlerror();
  addr = dlsym(handle, ManagledName);
  err = dlerror();
  if (!addr || err) {
    FATAL("getMyFuncAddr: dlsym(patch_so, %s) failed: %s", ManagledName,
          err ? err : "null");
  }
  const char *my_dso = getDSOName(addr);
  if (!llvm::StringRef(my_dso).endswith(patchName)) {
    FATAL("getMyFuncAddr: found symbol %s is not in patch so", my_dso);
  }
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
  void changeMemoryProtection(unsigned flags) {
    if (auto Ret = Memory::protectMappedMemory(Mem, flags)) {
      FATAL("Failed to protect memory for patching: %s", Ret.message().c_str());
    }
  }

public:
  unsigned getMemoryProtectionFlags(MemoryType type) {
    switch (type) {
    case MemoryType::Exec:
      return Memory::MF_EXEC | Memory::MF_READ;
    case MemoryType::RoData:
      return Memory::MF_READ;
    }
  }

  ScopedApplyPatch(void *FuncAddr, size_t n, MemoryType type = MemoryType::Exec)
      : Mem(FuncAddr, n), type(type) {
    changeMemoryProtection(Memory::MF_RWE_MASK);
  }
  ~ScopedApplyPatch() {
    changeMemoryProtection(getMemoryProtectionFlags(type));
  }

private:
  llvm::sys::MemoryBlock Mem;
  MemoryType type;
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
  if (unlikely(!isInitialized)) {
    FATAL("Cannot use patch without initialization");
  }
}

void XsanPatch::initialize(void *MyFunc, void *RealFunc) {
  std::memcpy(patch, PatchTempl, PatchSize);
  std::memcpy(&patch[FuncAddrOffset], (void *)&MyFunc, sizeof(void *));
  std::memcpy(backup, RealFunc, PatchSize);
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
