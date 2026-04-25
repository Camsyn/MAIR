
#include <clang/Basic/CodeGenOptions.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/Sanitizers.h>
#include <clang/Basic/TargetOptions.h>
#include <clang/CodeGen/BackendUtil.h>
#include <clang/Lex/HeaderSearchOptions.h>

#include "PatchCore.h"
#include "llvm/IR/Module.h"

using namespace clang;
using namespace llvm;
static __xsan::Interception Interceptee(&clang::EmitBackendOutput,
                                        {"clang", "clang++"});

void clang::EmitBackendOutput(DiagnosticsEngine &Diags,
                              const HeaderSearchOptions &HeaderOpts,
                              const CodeGenOptions &CGOpts,
                              const clang::TargetOptions &TOpts,
                              const LangOptions &LOpts, StringRef TDesc,
                              Module *M, BackendAction Action,
                              std::unique_ptr<raw_pwrite_stream> OS) {
  static constexpr SanitizerMask HackedSanitizers =
      SanitizerKind::Memory | SanitizerKind::Address | SanitizerKind::Thread;
  LangOptions NewLangOpts = LOpts;
  if (__xsan::isXsanEnabled()) {
    NewLangOpts.Sanitize.clear(HackedSanitizers);
  }
  Interceptee(Diags, HeaderOpts, CGOpts, TOpts, NewLangOpts, TDesc, M, Action,
              std::move(OS));
}
