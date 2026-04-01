#include <array>
#include <bitset>
#include <cctype>
#include <clang/Driver/ToolChain.h>
#include <cstddef>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/Option.h>
#include <optional>

#include "config_compile.h"
#include "debug.h"
#include "utils/PatchHelper.h"
#include "xsan_common.h"

namespace clang {
namespace driver {
namespace tools {
bool addSanitizerRuntimes(const ToolChain &TC, const llvm::opt::ArgList &Args,
                          llvm::opt::ArgStringList &CmdArgs);
}
} // namespace driver
} // namespace clang

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

using namespace __xsan;

static constexpr const std::array<llvm::StringRef, XSan + 1>
gen_hacked_san_names() {
  std::array<llvm::StringRef, XSan + 1> result = {};
  result[XSan] = "xsan";
  result[ASan] = "asan";
  result[TSan] = "tsan";
  result[MSan] = "msan";
  result[UBSan] = "ubsan_standalone";

  return result;
}

static constexpr auto HackedSanNames = gen_hacked_san_names();
// Save new args here to extend their lifespan.
static llvm::SmallVector<std::string, 8> SavedArgs;

namespace {

class HackedSanitizersRtRewriter {
  using StringRef = llvm::StringRef;

public:
  HackedSanitizersRtRewriter(ArgStringList &Args, const ToolChain &TC)
      : Args(Args), replace(getXsanMask().any()),
        shouldHasStaticRt(getSanType() == XSan || getSanType() == ASan),
        XsanRtDir(getXsanArchRtDir(TC.getTriple().str())),
        PrefixToReplace(XsanRtDir / "libclang_rt."),
        archSuffix("-" + TC.getArchName().str()), TC(TC) {
    if (getSanType() == SanNone || PrefixToReplace.empty())
      return;

    StringRef sanName = HackedSanNames[getSanType()];
    PrefixToReplace += sanName;

    generate_new_args();
    doSwap();
  }

private:
  void add_rpath(ArgStringList &CmdArgs) const {
    static const std::string rPathOpt = "-rpath=" + XsanRtDir.generic_string();
    CmdArgs.push_back(rPathOpt.c_str());
  }

  /// Match arg with *libclang_rt.<san>*.
  /// Return the position of the successful match + size of the pattern
  /// libclang_rt.<san>.
  static std::optional<size_t> findSanRtSuffix(StringRef arg) {
    size_t pos = arg.find(CompilerRt);
    if (pos == StringRef::npos)
      return std::nullopt;
    arg = arg.substr(pos + CompilerRt.size());
    for (StringRef sanName : HackedSanNames) {
      if (sanName.empty())
        continue;
      if (arg.startswith(sanName)) {
        return pos + CompilerRt.size() + sanName.size();
      }
    }
    return std::nullopt;
  }

  /// Only those link argument related to our hacked sanitizers
  /// should be replaced. Otherwise, we return nullopt.
  /// Requires : isHackedSanitizersRt(arg) == true
  std::optional<std::string> replaceSanitizerRt(StringRef arg) const {
    std::optional<size_t> pos_suffix = findSanRtSuffix(arg);
    if (!pos_suffix.has_value())
      return std::nullopt;
    size_t pos = pos_suffix.value();
    // Get the suffix, e.g.,
    //  1. *libclang_rt.asan-static.a -> -static.a
    //  2. *libclang_rt.asan.a.syms -> .a.syms
    StringRef suffix = arg.substr(pos);
    if (suffix.endswith(".so")) {
      if (getSanType() == XSan && getXsanMask().test(MSan)) {
        PFATAL("XSan with MSan does not support the shared runtime "
               "temporarily, as MSan does not support it either. Consider "
               "disable MSan from XSan if you want to use the shared runtime.");
      }
      pos = suffix.size() - 3;
    } else {
      pos = suffix.rfind(".a");
    }
    if (pos == StringRef::npos)
      return std::nullopt;

    // If suffix[:pos] ends with the archSuffix "-" + TC.getArchName(), remove
    // the archSuffix
    if (pos >= archSuffix.size()) {
      StringRef prefix = suffix.substr(0, pos);
      if (prefix.endswith(archSuffix)) {
        // --------------------------------------------------------------
        // Some non-standard link path might contains the archSuffix, as
        // follows:
        // .../lib/linux/libclang_rt.<san>-x86_64.a
        // Therefore, we need to remove the archSuffix.
        // --------------------------------------------------------------
        // Remove the archSuffix
        std::string rewrittenPath =
            /* <XSan-Path>/lib/<triple>/ */
            PrefixToReplace +
            /* libclang_rt.<san>-<arch> -> libclang_rt.<san> */
            prefix.substr(0, prefix.size() - archSuffix.size()).str() +
            /* .a/.so/.a.syms */
            suffix.substr(pos).str();
        return rewrittenPath;
      }
    }
    // Initialize the new RT as
    // <XSan-Path>/lib/<triple>/libclang_rt.<san><suffix>
    return PrefixToReplace + suffix.str();
  }

  /// Eat one arg, return false if the next arg should be skipped.
  bool eatOneArg(const char *arg) const {
    // [orig] xxx/libclang_rt.<san>.a --> [new XSan's] xxx/libclang_rt.<san>.a
    auto NewRt = replaceSanitizerRt(arg);
    // If NewRt is empty, that arg need not to be replaced.
    if (!NewRt.has_value()) {
      NewCmdArgs.push_back(arg);
      return true;
    }
    auto It = SeenRts.insert(NewRt.value());
    if (It.second) {
      // Record whether we have handled xxx_static.a.
      StringRef argref = arg;
      if (argref.contains("_static")) {
        shouldHasStaticRt = false;
      }
      // Handle --dynamic-list=
      if (NewCmdArgs.back() != PreRt && argref.startswith(DynList)) {
        NewRt = DynList.data() + NewRt.value();
      }

      // If so is replaced, we need to add rpath to the new runtime.
      if (argref.endswith(".so")) {
        add_rpath(NewCmdArgs);
      }

      // Extend the lifetime of NewRt
      SavedArgs.push_back(std::move(NewRt.value()));
      // Replace the link argument related to our hacked sanitizers
      NewCmdArgs.push_back(SavedArgs.back().c_str());
      return true;
    } else {
      // The relevant replaced argument was added, skip to avoid
      // duplicated appending.
      if (NewCmdArgs.back() == PreRt) {
        NewCmdArgs.pop_back();
        return false;
      }
    }
    return true;
  }

  void generate_new_args() const {
    bool skip_next = false;
    for (const auto Arg : Args) {
      if (skip_next) {
        if (Arg == PostRt) {
          skip_next = false;
        }
        continue;
      }
      skip_next = !eatOneArg(Arg);
    }

    // Fix `-xsan -fno-sanitize=address`
    // In this option, we should add xxx_static.a manually.
    if (shouldHasStaticRt) {
      shouldHasStaticRt = false;
      SavedArgs.push_back(PrefixToReplace + "_static" + ".a");
      NewCmdArgs.push_back(PreRt.data());
      NewCmdArgs.push_back(SavedArgs.back().c_str());
      NewCmdArgs.push_back(PostRt.data());
    }
  }

  void doSwap() { Args.swap(NewCmdArgs); }

  static bool hasStaticRuntime(StringRef arg);
  static constexpr StringRef CompilerRt = "libclang_rt.";
  static constexpr StringRef PreRt = "--whole-archive";
  static constexpr StringRef PostRt = "--no-whole-archive";
  static constexpr StringRef DynList = "--dynamic-list=";

  bool replace;
  mutable bool shouldHasStaticRt;

  const std::filesystem::path XsanRtDir;
  std::string PrefixToReplace;
  // e.g., "-x86_64"
  std::string archSuffix;
  ArgStringList &Args;

  mutable llvm::StringSet<> SeenRts;
  mutable ArgStringList NewCmdArgs;
  const ToolChain &TC;
};

static void add_wrap_link_option(ArgStringList &CmdArgs) {
  if (!isXsanEnabled() || getSanType() != XSan) {
    return;
  }
  static const std::string WrapSymbolLinkOpt =
      "@" +
      (getXsanAbsPath(XSAN_SHARE_DIR "/" + getXsanCombName(getXsanMask())) /
       "xsan_wrapped_symbols.txt")
          .generic_string();
  CmdArgs.push_back(WrapSymbolLinkOpt.c_str());
}

} // namespace

static XsanInterceptor Interceptor(tools::addSanitizerRuntimes,
                                   {"clang", "clang++"});

// Should be called before we add system libraries (C++ ABI, libstdc++/libc++,
// C runtime, etc). Returns true if sanitizer system deps need to be linked in.
bool tools::addSanitizerRuntimes(const ToolChain &TC, const ArgList &Args,
                                 ArgStringList &CmdArgs) {
  bool result = Interceptor(TC, Args, CmdArgs);
  if (isXsanEnabled()) {
    HackedSanitizersRtRewriter Rewriter(CmdArgs, TC);
    add_wrap_link_option(CmdArgs);
  }
  return result;
}
