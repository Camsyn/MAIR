#pragma once

#include "XSanDeSanReSan.h"

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
#include <string>
#include <vector>

namespace llvm {
struct AddressSanitizerOptions;
enum class AsanDtorKind;
} // namespace llvm

namespace __xsan {

class MPMRewriter;

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
  void addSanitizerWithDeSan(llvm::StringRef SanName, PassAdder AddSanitizer);
  void addDeSanPass(llvm::StringRef SanName);
  void addReSanPass();

  llvm::OptimizationLevel Level;
  llvm::SmallSetVector<unsigned, 8> DeSanPassIndices;
  int FirstSanitizerPassIndex = -1;
  int ReSanPassIndex = -1;
  std::shared_ptr<desan::SanitizingContext> Ctx;
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

// --------- Build sanitizer pass groups in an existing MPM -----------
// The basic logic of add*sanToMPM comes from addSanitizer@BackendUtil.cpp
void addAsanToMPM(llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level);
void addTsanToMPM(llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level);
void addMsanToMPM(llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level);
void addXsanToMPM(llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level);
void addXsanPipelineStartPasses(llvm::ModulePassManager &MPM);

// The livepatch rewriter is intentionally sanitizer-agnostic: each pass plugin
// registers the callbacks that own each insertion site.  The rewriter only
// accesses the sanitizer-position adder through friendship; callers use the
// explicit registration API below.
class PassRegistry {
public:
  using PassAdder = void (*)(llvm::ModulePassManager &,
                             llvm::OptimizationLevel);

  void registerPassIntoOpt(llvm::StringRef Name, PassAdder AddPass);
  void registerStartPassIntoClang(PassAdder AddPass);
  void registerSanitizerPassIntoClang(PassAdder AddPass);

  void delegateNonSanPassTo(llvm::PassBuilder &PB);

private:
  friend class MPMRewriter;

  struct NamedPassAdder {
    std::string Name;
    PassAdder AddPass;
  };

  std::vector<NamedPassAdder> OptPassAdders;
  std::vector<PassAdder> StartPassAdders;
  bool NonSanPassDelegated = false;
  bool HasSanitizerPassAdder = false;
  PassAdder SanitizerPassAdder = nullptr;
};

extern PassRegistry PassRegistry;

// --------- Implemented in PassRegistry.cpp -----------
void registerAsanForClangAndOpt(llvm::PassBuilder &PB);
void registerTsanForClangAndOpt(llvm::PassBuilder &PB);
void registerMsanForClangAndOpt(llvm::PassBuilder &PB);

// --------- Use for XSan's optimization ---------------
bool shouldAsanPoisonInternalGlobals();
} // namespace __xsan
