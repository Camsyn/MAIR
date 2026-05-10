#pragma once

#include "XSanSanitizingContext.h"
#include "livepatch/record/SanitizingRecorder.h"

#include "llvm/IR/PassManager.h"

#include <memory>

namespace __xsan::desan {

class PreprocessPass : public llvm::PassInfoMixin<PreprocessPass> {
public:
  explicit PreprocessPass(std::shared_ptr<SanitizingContext> Ctx);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  std::shared_ptr<SanitizingContext> Ctx;
};

class UserTaggingPass : public llvm::PassInfoMixin<UserTaggingPass> {
public:
  explicit UserTaggingPass(std::shared_ptr<SanitizingContext> Ctx);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  std::shared_ptr<SanitizingContext> Ctx;
};

class DeSanPass : public llvm::PassInfoMixin<DeSanPass> {
public:
  DeSanPass(llvm::StringRef SanName, std::shared_ptr<SanitizingContext> Ctx);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  const Sanitizer San;
  std::shared_ptr<SanitizingContext> Ctx;
};

class ReSanPass : public llvm::PassInfoMixin<ReSanPass> {
public:
  explicit ReSanPass(std::shared_ptr<SanitizingContext> Ctx);

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  std::shared_ptr<SanitizingContext> Ctx;
};

} // namespace __xsan::desan
