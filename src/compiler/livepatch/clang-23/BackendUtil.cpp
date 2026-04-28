#include "clang/CodeGen/BackendUtil.h"
#include "clang/Basic/Sanitizers.h"
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

  if (__xsan::isXsanEnabled()) {
    CI.getLangOpts().Sanitize.clear(HackedSanitizers);
    // Keep a single original ASan pass as a pipeline anchor.  The XSan pass
    // plugin removes this anchor from the final ModulePassManager and inserts
    // the selected XSan/custom sanitizer pass group at the same position.  This
    // preserves Clang's sanitizer ordering, especially SanCov before sanitizer
    // instrumentation, without letting the original ASan pass run.
    CI.getLangOpts().Sanitize.set(SanitizerKind::Address, true);
  }

  Interceptee(CI, CGOpts, TDesc, M, Action, VFS, std::move(OS), BC);
}
