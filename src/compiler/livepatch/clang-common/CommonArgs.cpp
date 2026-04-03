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

#include "CommonArgs.h"
#include "config_compile.h"
#include "debug.h"
#include "utils/PatchHelper.h"
#include "xsan_common.h"
#include "xsan_llvm_compat.h"

using namespace clang::driver;
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

  static std::optional<size_t> findSanRtSuffix(StringRef arg) {
    size_t pos = arg.find(CompilerRt);
    if (pos == StringRef::npos)
      return std::nullopt;
    arg = arg.substr(pos + CompilerRt.size());
    for (StringRef sanName : HackedSanNames) {
      if (sanName.empty())
        continue;
      if (compat::startsWith(arg, sanName))
        return pos + CompilerRt.size() + sanName.size();
    }
    return std::nullopt;
  }

  std::optional<std::string> replaceSanitizerRt(StringRef arg) const {
    std::optional<size_t> pos_suffix = findSanRtSuffix(arg);
    if (!pos_suffix.has_value())
      return std::nullopt;
    size_t pos = pos_suffix.value();
    StringRef suffix = arg.substr(pos);
    if (compat::endsWith(suffix, ".so")) {
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

    if (pos >= archSuffix.size()) {
      StringRef prefix = suffix.substr(0, pos);
      if (compat::endsWith(prefix, archSuffix)) {
        std::string rewrittenPath =
            PrefixToReplace +
            prefix.substr(0, prefix.size() - archSuffix.size()).str() +
            suffix.substr(pos).str();
        return rewrittenPath;
      }
    }
    return PrefixToReplace + suffix.str();
  }

  bool eatOneArg(const char *arg) const {
    auto NewRt = replaceSanitizerRt(arg);
    if (!NewRt.has_value()) {
      NewCmdArgs.push_back(arg);
      return true;
    }
    auto It = SeenRts.insert(NewRt.value());
    if (It.second) {
      StringRef argref = arg;
      if (argref.contains("_static")) {
        shouldHasStaticRt = false;
      }
      if (NewCmdArgs.back() != PreRt &&
          compat::startsWith(argref, DynList)) {
        NewRt = DynList.data() + NewRt.value();
      }

      if (compat::endsWith(argref, ".so")) {
        add_rpath(NewCmdArgs);
      }

      SavedArgs.push_back(std::move(NewRt.value()));
      NewCmdArgs.push_back(SavedArgs.back().c_str());
      return true;
    }

    if (NewCmdArgs.back() == PreRt) {
      NewCmdArgs.pop_back();
      return false;
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
  std::string archSuffix;
  ArgStringList &Args;

  mutable llvm::StringSet<> SeenRts;
  mutable ArgStringList NewCmdArgs;
  const ToolChain &TC;
};

static void add_wrap_link_option(ArgStringList &CmdArgs) {
  if (!isXsanEnabled() || getSanType() != XSan)
    return;
  static const std::string WrapSymbolLinkOpt =
      "@" +
      (getXsanAbsPath(XSAN_SHARE_DIR "/" + getXsanCombName(getXsanMask())) /
       "xsan_wrapped_symbols.txt")
          .generic_string();
  CmdArgs.push_back(WrapSymbolLinkOpt.c_str());
}

} // namespace

namespace __xsan::livepatch {

void rewriteSanitizerRuntimeArgs(ArgStringList &CmdArgs, const ToolChain &TC) {
  HackedSanitizersRtRewriter Rewriter(CmdArgs, TC);
  add_wrap_link_option(CmdArgs);
}

} // namespace __xsan::livepatch
