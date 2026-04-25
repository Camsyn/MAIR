#include "clang/Basic/Sanitizers.h"
#include "clang/CodeGen/BackendUtil.h"
#include "clang/Frontend/CompilerInstance.h"

#include "PatchCore.h"

using namespace clang;
using namespace llvm;

static __xsan::Interception Interceptee(&clang::emitBackendOutput,
                                        {"clang", "clang++"});

void clang::emitBackendOutput(CompilerInstance &CI, CodeGenOptions &CGOpts,
                              StringRef TDesc, llvm::Module *M,
                              BackendAction Action,
                              IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                              std::unique_ptr<raw_pwrite_stream> OS,
                              BackendConsumer *BC) {
  static constexpr SanitizerMask HackedSanitizers =
      SanitizerKind::Memory | SanitizerKind::Address | SanitizerKind::Thread;

  if (__xsan::isXsanEnabled())
    CI.getLangOpts().Sanitize.clear(HackedSanitizers);

  Interceptee(CI, CGOpts, TDesc, M, Action, VFS, std::move(OS), BC);
}
