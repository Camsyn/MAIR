// Livepatch the LLVM PassBuilder pipeline construction used by
// *SanInstPass.so.
// The corresponding LLVM source is
//      llvm/lib/Passes/PassBuilderPipelines.cpp
// this file rewrites the final instrumentation plugin MPM so
// custom sanitizer passes occupy Clang's original sanitizer position.

#include "PassRegistry.h"
#include "PatchCore.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <optional>
#include <utility>

using namespace llvm;

namespace __xsan {

class MPMRewriter : public ModulePassManager {
public:
  MPMRewriter() = default;
  explicit MPMRewriter(ModulePassManager &&MPM)
      : ModulePassManager(std::move(MPM)) {}

  MPMRewriter(MPMRewriter &&) = default;
  MPMRewriter &operator=(MPMRewriter &&) = default;

  bool rewrite(OptimizationLevel Level) {
    std::optional<unsigned> Anchor = findAsanAnchorIndex();
    if (!Anchor)
      return false;

    MPMRewriter Suffix = takeSuffix(*Anchor + 1);
    // Drop the original ASan anchor itself.  It exists only so that we can
    // recover Clang's sanitizer position after SanCov has been inserted.
    Passes.resize(*Anchor);

    if (!addReplacementPasses(Level))
      FATAL("XSan found Clang's sanitizer anchor but no sanitizer-position "
            "replacement callback was registered by the pass plugin.");

    append(std::move(Suffix));
    return true;
  }

  std::string pipelineString() {
    std::string Pipeline;
    raw_string_ostream OS(Pipeline);
    printPipeline(OS, [](StringRef PassName) { return PassName; });
    return OS.str();
  }

private:
  std::optional<unsigned> findAsanAnchorIndex() const {
    for (unsigned Idx = 0, Size = Passes.size(); Idx != Size; ++Idx) {
      StringRef Name = Passes[Idx]->name();
      if (Name == "AddressSanitizerPass" ||
          Name == "ModuleAddressSanitizerPass")
        return Idx;
    }
    return std::nullopt;
  }

  MPMRewriter takeSuffix(unsigned First) {
    MPMRewriter Suffix;
    for (unsigned Idx = First, Size = Passes.size(); Idx != Size; ++Idx)
      Suffix.Passes.push_back(std::move(Passes[Idx]));
    Passes.resize(First);
    return Suffix;
  }

  void append(MPMRewriter &&Other) {
    for (auto &Pass : Other.Passes)
      Passes.push_back(std::move(Pass));
    Other.Passes.clear();
  }

  bool addReplacementPasses(OptimizationLevel Level) {
    // Do not branch on SanitizerType here.  The rewriter's only responsibility
    // is to recover Clang's original sanitizer position after SanCov.  The
    // loaded pass plugin owns the policy for what should be inserted at that
    // position by registering an indirect callback in the global PassRegistry.
    if (!PassRegistry.HasSanitizerPassAdder)
      return false;

    if (PassRegistry.SanitizerPassAdder)
      PassRegistry.SanitizerPassAdder(*this, Level);
    return true;
  }
};

} // namespace __xsan

namespace {

thread_local unsigned PipelineBuildDepth = 0;

class PipelineBuildScope {
public:
  PipelineBuildScope() { ++PipelineBuildDepth; }
  ~PipelineBuildScope() { --PipelineBuildDepth; }

  bool isOutermost() const { return PipelineBuildDepth == 1; }
};

ModulePassManager rewriteSanitizerPipeline(ModulePassManager &&MPM,
                                           OptimizationLevel Level) {
  if (!__xsan::isXsanEnabled())
    return std::move(MPM);

  __xsan::MPMRewriter Rewriter(std::move(MPM));
  if (Rewriter.rewrite(Level))
    return Rewriter;

  std::string Pipeline = Rewriter.pipelineString();
  FATAL("XSan expected clang to build an AddressSanitizer anchor pass after "
        "SanCov, but no AddressSanitizerPass/ModuleAddressSanitizerPass was "
        "found in the final module pipeline. Current pipeline: %s",
        Pipeline.c_str());
}

} // namespace

static constexpr std::array<const char *, 2> TargetProcesses{"clang",
                                                             "clang++"};

#if LLVM_VERSION_MAJOR >= 22

static ModulePassManager buildPerModuleDefaultPipelineInterceptor(
    PassBuilder *PB, OptimizationLevel Level, ThinOrFullLTOPhase Phase);
static ModulePassManager
buildLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                          OptimizationLevel Level);
static ModulePassManager
buildThinLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                              OptimizationLevel Level);
static ModulePassManager
buildO0DefaultPipelineInterceptor(PassBuilder *PB, OptimizationLevel Level,
                                  ThinOrFullLTOPhase Phase);

static __xsan::Interception buildPerModuleDefaultPipelineInterceptee(
    &buildPerModuleDefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder29buildPerModuleDefaultPipelineENS_"
    "17OptimizationLevelENS_18ThinOrFullLTOPhaseE",
    TargetProcesses);
static __xsan::Interception buildLTOPreLinkDefaultPipelineInterceptee(
    &buildLTOPreLinkDefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder30buildLTOPreLinkDefaultPipelineENS_"
    "17OptimizationLevelE",
    TargetProcesses);
static __xsan::Interception buildThinLTOPreLinkDefaultPipelineInterceptee(
    &buildThinLTOPreLinkDefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder34buildThinLTOPreLinkDefaultPipelineENS_"
    "17OptimizationLevelE",
    TargetProcesses);
static __xsan::Interception buildO0DefaultPipelineInterceptee(
    &buildO0DefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder22buildO0DefaultPipelineENS_17OptimizationLevelENS_"
    "18ThinOrFullLTOPhaseE",
    TargetProcesses);

static ModulePassManager buildPerModuleDefaultPipelineInterceptor(
    PassBuilder *PB, OptimizationLevel Level, ThinOrFullLTOPhase Phase) {
  PipelineBuildScope Scope;
  ModulePassManager MPM =
      buildPerModuleDefaultPipelineInterceptee(PB, Level, Phase);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

static ModulePassManager
buildLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                          OptimizationLevel Level) {
  PipelineBuildScope Scope;
  ModulePassManager MPM = buildLTOPreLinkDefaultPipelineInterceptee(PB, Level);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

static ModulePassManager
buildThinLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                              OptimizationLevel Level) {
  PipelineBuildScope Scope;
  ModulePassManager MPM =
      buildThinLTOPreLinkDefaultPipelineInterceptee(PB, Level);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

static ModulePassManager
buildO0DefaultPipelineInterceptor(PassBuilder *PB, OptimizationLevel Level,
                                  ThinOrFullLTOPhase Phase) {
  PipelineBuildScope Scope;
  ModulePassManager MPM = buildO0DefaultPipelineInterceptee(PB, Level, Phase);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

#else

static ModulePassManager buildPerModuleDefaultPipelineInterceptor(
    PassBuilder *PB, OptimizationLevel Level, bool LTOPreLink);
static ModulePassManager
buildLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                          OptimizationLevel Level);
static ModulePassManager
buildThinLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                              OptimizationLevel Level);
static ModulePassManager
buildO0DefaultPipelineInterceptor(PassBuilder *PB, OptimizationLevel Level,
                                  bool LTOPreLink);

static __xsan::Interception buildPerModuleDefaultPipelineInterceptee(
    &buildPerModuleDefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder29buildPerModuleDefaultPipelineENS_"
    "17OptimizationLevelEb",
    TargetProcesses);
static __xsan::Interception buildLTOPreLinkDefaultPipelineInterceptee(
    &buildLTOPreLinkDefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder30buildLTOPreLinkDefaultPipelineENS_"
    "17OptimizationLevelE",
    TargetProcesses);
static __xsan::Interception buildThinLTOPreLinkDefaultPipelineInterceptee(
    &buildThinLTOPreLinkDefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder34buildThinLTOPreLinkDefaultPipelineENS_"
    "17OptimizationLevelE",
    TargetProcesses);
static __xsan::Interception buildO0DefaultPipelineInterceptee(
    &buildO0DefaultPipelineInterceptor,
    "_ZN4llvm11PassBuilder22buildO0DefaultPipelineENS_17OptimizationLevelEb",
    TargetProcesses);

static ModulePassManager buildPerModuleDefaultPipelineInterceptor(
    PassBuilder *PB, OptimizationLevel Level, bool LTOPreLink) {
  PipelineBuildScope Scope;
  ModulePassManager MPM =
      buildPerModuleDefaultPipelineInterceptee(PB, Level, LTOPreLink);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

static ModulePassManager
buildLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                          OptimizationLevel Level) {
  PipelineBuildScope Scope;
  ModulePassManager MPM = buildLTOPreLinkDefaultPipelineInterceptee(PB, Level);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

static ModulePassManager
buildThinLTOPreLinkDefaultPipelineInterceptor(PassBuilder *PB,
                                              OptimizationLevel Level) {
  PipelineBuildScope Scope;
  ModulePassManager MPM =
      buildThinLTOPreLinkDefaultPipelineInterceptee(PB, Level);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

static ModulePassManager
buildO0DefaultPipelineInterceptor(PassBuilder *PB, OptimizationLevel Level,
                                  bool LTOPreLink) {
  PipelineBuildScope Scope;
  ModulePassManager MPM =
      buildO0DefaultPipelineInterceptee(PB, Level, LTOPreLink);
  if (!Scope.isOutermost())
    return std::move(MPM);
  return rewriteSanitizerPipeline(std::move(MPM), Level);
}

#endif
