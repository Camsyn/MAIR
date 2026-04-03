#pragma once

#include <clang/Driver/ToolChain.h>
#include <llvm/Option/ArgList.h>

namespace __xsan::livepatch {

void rewriteSanitizerRuntimeArgs(llvm::opt::ArgStringList &CmdArgs,
                                 const clang::driver::ToolChain &TC);

} // namespace __xsan::livepatch
