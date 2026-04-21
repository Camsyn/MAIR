#include "XSanDeSanPass.h"

#include "Utils/Options.h"
#include "Utils/UbsanUtils.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

namespace __xsan::desan {

PreprocessPass::PreprocessPass(std::shared_ptr<PipelineState> State)
    : State(std::move(State)) {}

PreservedAnalyses PreprocessPass::run(Module &M, ModuleAnalysisManager &MAM) {
  requireOrDie(!!State, "desan preprocess pass missing pipeline state");

  resetSession();

  // Do dead code elimination first to avoid sanitizer remove user basic blocks,
  // which we can't deal with for now.
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (Function &F : M) {
    if (!F.isDeclaration() && llvm::removeUnreachableBlocks(F)) {
      while (llvm::removeUnreachableBlocks(F))
        ;
      FAM.invalidate(F, PreservedAnalyses::none());
    }
  }

  State->Archive = ReSanArchive();
  State->UserBBs = collectUserBBs(M);
  markUserIR(M);

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS("before.ll", EC, llvm::sys::fs::OF_None);
    M.print(OS, nullptr);
  }

  return PreservedAnalyses::all();
}

StagePass::StagePass(StageKind Stage, StageRunner Runner)
    : Stage(Stage), Runner(std::move(Runner)) {}

PreservedAnalyses StagePass::run(Module &M, ModuleAnalysisManager &AM) {
  requireOrDie(!!Runner, Twine("desan stage pass missing runner for stage ") +
                             getStageName(Stage));

  beginStage(Stage);
  PreservedAnalyses PA = Runner(M, AM);
  endStage();
  return PA;
}

DeSanPass::DeSanPass(StageKind Stage, std::shared_ptr<PipelineState> State)
    : Stage(Stage), State(std::move(State)) {}

PreservedAnalyses DeSanPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!State, "desan restore pass missing pipeline state");

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(
        (llvm::Twine(getStageName(Stage)) + ".before.ll").str(), EC,
        llvm::sys::fs::OF_None);
    M.print(OS, nullptr);
  }

  runDeSan(M, Stage, State->Archive, State->UserBBs);

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(
        (llvm::Twine(getStageName(Stage)) + ".after.ll").str(), EC,
        llvm::sys::fs::OF_None);
    M.print(OS, nullptr);
  }

  return PreservedAnalyses::all();
}

ReSanPass::ReSanPass(std::shared_ptr<PipelineState> State)
    : State(std::move(State)) {}

PreservedAnalyses ReSanPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!State, "resan replay pass missing pipeline state");

  if (options::ClDebug)
    validateArchive(State->Archive, State->UserBBs);

  runReSan(M, State->Archive);

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS("resan.ll", EC, llvm::sys::fs::OF_None);
    M.print(OS, nullptr);
  }

  return PreservedAnalyses::all();
}

} // namespace __xsan::desan
