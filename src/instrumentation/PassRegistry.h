#pragma once

#include "XSanDeSanPass.h"

#include "llvm/ADT/SetVector.h"
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
#include <memory>

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

  SubSanitizers(llvm::OptimizationLevel Level) : Level(Level) {}

  bool isDeSanPass(unsigned PassIndex) const {
    return DeSanPassIndices.contains(PassIndex);
  }
  bool isInSanitizing(unsigned PassIndex) const;
  void beginPass(unsigned PassIndex);
  void resetRunState();
  void addSanitizerWithDeSan(const desan::Sanitizer &San,
                             PassAdder AddSanitizer);
  void addDeSanPass(const desan::Sanitizer &San);
  void addReSanPass();

  llvm::OptimizationLevel Level;
  llvm::SmallSetVector<unsigned, 8> DeSanPassIndices;
  int FirstSanitizerPassIndex = -1;
  int ReSanPassIndex = -1;
  std::shared_ptr<desan::SanitizingContext> State;
  bool Recording = false;

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
