#include <clang/Driver/ToolChain.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Option/ArgList.h>

#include "../clang-common/CommonArgs.h"
#include "PatchCore.h"

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

static Interception Interceptee(tools::addSanitizerRuntimes,
                                {"clang", "clang++"});

// Should be called before we add system libraries (C++ ABI, libstdc++/libc++,
// C runtime, etc). Returns true if sanitizer system deps need to be linked in.
bool tools::addSanitizerRuntimes(const ToolChain &TC, const ArgList &Args,
                                 ArgStringList &CmdArgs) {
  bool result = Interceptee(TC, Args, CmdArgs);
  if (isXsanEnabled())
    __xsan::livepatch::rewriteSanitizerRuntimeArgs(CmdArgs, TC);
  return result;
}
