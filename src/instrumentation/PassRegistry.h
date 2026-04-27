#pragma once

#include "XSanDeSanPass.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#if LLVM_VERSION_MAJOR >= 20
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizerOptions.h"
#include "llvm/Transforms/Instrumentation/MemorySanitizer.h"

#include <functional>
#include <optional>

namespace llvm {
struct AddressSanitizerOptions;
enum class AsanDtorKind;
} // namespace llvm

namespace __xsan {

struct AsanOption {
  llvm::AddressSanitizerOptions Opts;
  bool UseGlobalGC;
  bool UseOdrIndicator;
  llvm::AsanDtorKind DestructorKind;
};

struct MsanOption {
  llvm::MemorySanitizerOptions Opts;

  MsanOption(int TrackOrigins, bool Recover, bool Kernel, bool EagerChecks)
      : Opts(TrackOrigins, Recover, Kernel, EagerChecks) {}
};

class SubSanitizers : public llvm::ModulePassManager {
private:
  using PassAdder =
      std::function<void(llvm::ModulePassManager &, llvm::OptimizationLevel)>;

  class ScopedStageTransition {
  public:
    ScopedStageTransition(unsigned PassIndex, SubSanitizers &Owner);
    ~ScopedStageTransition();

  private:
    const unsigned PassIndex;
    const bool IsDeSanPass;
    SubSanitizers &Owner;
  };

  SubSanitizers(llvm::OptimizationLevel Level) : Level(Level) {}

  void advanceStage();
  std::optional<desan::StageKind> currentStage() const;
  bool isDeSanPass(unsigned PassIndex) const {
    return DeSanPassIndices.contains(PassIndex);
  }
  bool isInSanitzing(unsigned PassIndex) const;
  void resetRunState();
  void addSanitizerWithDeSan(desan::StageKind Stage, PassAdder AddSanitizer);
  void addDeSanPass(desan::StageKind Stage);
  void addReSanPass();

  llvm::OptimizationLevel Level;
  llvm::SmallSetVector<unsigned, 8> DeSanPassIndices;
  int StageEntryPassIndex = -1;
  int ReSanPassIndex = -1;
  llvm::SmallVector<desan::StageKind, 8> Stages;
  size_t StageIndex = 0;
  std::shared_ptr<desan::SanitizingContext> State;
  bool InStage = false;

public:
  static SubSanitizers loadSubSanitizers(llvm::OptimizationLevel Level);

  /// Run all of the sanitizer passes on the module, and preserve all analysises
  /// between them.
  llvm::PreservedAnalyses run(llvm::Module &IR,
                              llvm::ModuleAnalysisManager &AM);
};

/*
 A straightforward approach is to reuse the O1/O2/O3 optimization pipeline,
 which performs a excellent optimization on the code. However, this approach
 will bring compilation overhead, and most optimizations are repeated
 pointlessly.

 We just need to add some optimization passes designated for sanitizers!

 Therefore, we manually selected some low-cost and effective optimization
 passes.
 */
llvm::ModulePassManager
createPostOptimizationPasses(llvm::OptimizationLevel Level);

const AsanOption obtainAsanPassArgs();
const MsanOption obtainMsanPassArgs();

// --------- Implemented in PassRegistry.cpp -----------
void registerAsanForClangAndOpt(llvm::PassBuilder &PB);
void registerTsanForClangAndOpt(llvm::PassBuilder &PB);
void registerMsanForClangAndOpt(llvm::PassBuilder &PB);

// --------- Use for XSan's optimization ---------------
bool shouldAsanPoisonInternalGlobals();
} // namespace __xsan
