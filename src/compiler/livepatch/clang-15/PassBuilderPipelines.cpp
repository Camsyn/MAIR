/*
Hot patch to modify the sanitizer passes pipeline
*/

#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Target/TargetMachine.h>

#include <utility>

#include "PatchCore.h"

using namespace llvm;
using namespace __xsan;

namespace {

const StringSet<> HackedModulePasses = {"ModuleAddressSanitizerPass",
                                        "ModuleMemorySanitizerPass",
                                        "ModuleThreadSanitizerPass"};
const StringSet<> HackedFunctionPasses = {"ThreadSanitizerPass",
                                          "MemorySanitizerPass"};

void verifyOriginalPassNotExists(StringRef PassName) {
  if (!HackedModulePasses.contains(PassName))
    return;
  FATAL("Sanitizer's original pass %s is registered "
        "unexpectedly.\n\n" ERR_MSG_4_ORIG_PASS,
        PassName.str().c_str());
}

class MPMRewriter : public ModulePassManager {
public:
  MPMRewriter(ModulePassManager &&MPM)
      : ModulePassManager(std::forward<ModulePassManager>(MPM)) {

    /// TODO: Remove and add passes here
    for (auto &P : Passes) {
      // if (P->name() == "ModuleToFunctionPassAdaptor") {
      //   auto *p = (ModuleToFunctionPassAdaptor *)&P;
      //   p->printPipeline(outs(), [](StringRef PassName) { return PassName;
      //   }); outs() << "\n";
      // }
      // OKF("Pass: %s", P->name().str().c_str());
      ::verifyOriginalPassNotExists(P->name());
    }
  }

private:
};

ModulePassManager modifySanitizerPassesPipeline(ModulePassManager &MPM) {
  if (!isXsanEnabled() || !getXsanMask()[XSan])
    return std::move(MPM);
  ModulePassManager NewMPM;
  ::MPMRewriter Rewriter(std::move(MPM));
  return Rewriter;
}
} // namespace

static Interception buildPerModuleDefaultPipelineInterceptee(
    &PassBuilder::buildPerModuleDefaultPipeline, {"clang", "clang++"});
static Interception buildLTOPreLinkDefaultPipelineInterceptee(
    &PassBuilder::buildLTOPreLinkDefaultPipeline, {"clang", "clang++"});
static Interception buildThinLTOPreLinkDefaultPipelineInterceptee(
    &PassBuilder::buildThinLTOPreLinkDefaultPipeline, {"clang", "clang++"});
static Interception
    buildO0DefaultPipelineInterceptee(&PassBuilder::buildO0DefaultPipeline,
                                      {"clang", "clang++"});

ModulePassManager
PassBuilder::buildPerModuleDefaultPipeline(OptimizationLevel Level,
                                           bool LTOPreLink) {
  ModulePassManager MPM =
      buildPerModuleDefaultPipelineInterceptee(this, Level, LTOPreLink);
  return ::modifySanitizerPassesPipeline(MPM);
}

ModulePassManager
PassBuilder::buildLTOPreLinkDefaultPipeline(OptimizationLevel Level) {
  ModulePassManager MPM =
      buildLTOPreLinkDefaultPipelineInterceptee(this, Level);
  return ::modifySanitizerPassesPipeline(MPM);
}

ModulePassManager
PassBuilder::buildThinLTOPreLinkDefaultPipeline(OptimizationLevel Level) {
  ModulePassManager MPM =
      buildThinLTOPreLinkDefaultPipelineInterceptee(this, Level);
  return ::modifySanitizerPassesPipeline(MPM);
}

ModulePassManager PassBuilder::buildO0DefaultPipeline(OptimizationLevel Level,
                                                      bool LTOPreLink) {
  ModulePassManager MPM =
      buildO0DefaultPipelineInterceptee(this, Level, LTOPreLink);
  return ::modifySanitizerPassesPipeline(MPM);
}
