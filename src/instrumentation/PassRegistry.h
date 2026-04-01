#pragma once

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
  SubSanitizers() = default;

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

// -- The following funtcions are implemented in XXXSanitizer.cpp --------
/// Because the structure, indicating targets to instrument, depends on the
/// interior type in each XXXSanitizer.cpp
struct AsanToInstrument;
struct TsanToInstrument;
void registerAnalysisForAsan(llvm::PassBuilder &PB);
void registerAnalysisForTsan(llvm::PassBuilder &PB);
void registerAnalysisForMsan(llvm::PassBuilder &PB);
void registerAnalysisForXsan(llvm::PassBuilder &PB);
void addAsanRequireAnalysisPass(SubSanitizers &MPM,
                                llvm::FunctionPassManager &FPM);
void addTsanRequireAnalysisPass(SubSanitizers &MPM,
                                llvm::FunctionPassManager &FPM);
void addMsanRequireAnalysisPass(SubSanitizers &MPM,
                                llvm::FunctionPassManager &FPM);

// --------- Implemented in PassRegistry.cpp -----------
void registerAsanForClangAndOpt(llvm::PassBuilder &PB);
void registerTsanForClangAndOpt(llvm::PassBuilder &PB);
void registerMsanForClangAndOpt(llvm::PassBuilder &PB);

// --------- Use for XSan's optimization ---------------
bool shouldAsanPoisonInternalGlobals();
} // namespace __xsan
