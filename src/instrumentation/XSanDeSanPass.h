#pragma once

#include "XSanDeSanState.h"
#include "livepatch/XSanSanitizingRecorder.h"

#include "llvm/IR/PassManager.h"

#include <functional>
#include <memory>

namespace __xsan::desan {

class PreprocessPass : public llvm::PassInfoMixin<PreprocessPass> {
public:
  explicit PreprocessPass(std::shared_ptr<SanitizingContext> State);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  std::shared_ptr<SanitizingContext> State;
};

class StagePass : public llvm::PassInfoMixin<StagePass> {
public:
  using StageRunner = std::function<llvm::PreservedAnalyses(
      llvm::Module &, llvm::ModuleAnalysisManager &)>;

  StagePass(StageKind Stage, StageRunner Runner);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  StageKind Stage;
  StageRunner Runner;
};

class DeSanPass : public llvm::PassInfoMixin<DeSanPass> {
public:
  DeSanPass(StageKind Stage, std::shared_ptr<SanitizingContext> State);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  StageKind Stage;
  std::shared_ptr<SanitizingContext> State;
};

class ReSanPass : public llvm::PassInfoMixin<ReSanPass> {
public:
  explicit ReSanPass(std::shared_ptr<SanitizingContext> State);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  std::shared_ptr<SanitizingContext> State;
};

} // namespace __xsan::desan
