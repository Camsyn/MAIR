#include "PassRegistry.h"
#include "AttributeTaggingPass.hpp"
#include "PatchCore.h"
#include "Utils/Options.h"
#include "XSanDeSanPass.h"
#include "livepatch/XSanSanitizingRecorder.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizerOptions.h"
#include "llvm/Transforms/Instrumentation/ThreadSanitizer.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/LoopLoadElimination.h"
#include "llvm/Transforms/Scalar/LoopSink.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/TailRecursionElimination.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#if LLVM_VERSION_MAJOR >= 23
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Transforms/Scalar/JumpThreading.h"
#endif

using namespace llvm;

// -- These middle-end options are used to simulate the frontend options --

/// Set the default value to true
/// From
/// https://github.com/llvm/llvm-project/commit/1ada819c237bf724e6eaa1c82b2742e3eb57a5d5#diff-3fdc9c7b499c6c90240f4409c3ad6bd18cc9b0c247751ab371769101a195238b
static cl::opt<bool> ClAsanUseOdrIndicator(
    "sanitize-address-use-odr-indicator",
    cl::desc("Simulates -fsanitize-address-use-odr-indicator"), cl::Hidden,
    cl::init(true));

static cl::opt<AsanDtorKind> ClAsanDestructorKind(
    "sanitize-address-destructor",
    cl::desc("Simulates -fsanitize-address-destructor"),
    cl::values(clEnumValN(AsanDtorKind::None, "none", "No destructors"),
               clEnumValN(AsanDtorKind::Global, "global",
                          "Use global destructors")),
    cl::init(AsanDtorKind::Global), cl::Hidden);

static cl::opt<bool> ClAsanUseAfterScope(
    "sanitize-address-use-after-scope",
    cl::desc("Simulates -fsanitize-address-use-after-scope"), cl::Hidden,
    cl::init(true));

static cl::opt<AsanDetectStackUseAfterReturnMode> ClAsanUseAfterReturn(
    "sanitize-address-use-after-return",
    cl::desc("Simulates -fsanitize-address-use-after-return"),
    cl::values(
        clEnumValN(AsanDetectStackUseAfterReturnMode::Never, "never",
                   "Never detect stack use after return."),
        clEnumValN(
            AsanDetectStackUseAfterReturnMode::Runtime, "runtime",
            "Detect stack use after return if "
            "binary flag 'ASAN_OPTIONS=detect_stack_use_after_return' is set."),
        clEnumValN(AsanDetectStackUseAfterReturnMode::Always, "always",
                   "Always detect stack use after return.")),
    cl::Hidden, cl::init(AsanDetectStackUseAfterReturnMode::Runtime));

/// In LLVM 15, this option is false by default.
//    As a workaround for a bug in gold 2.26 and earlier, dead stripping of
//    globals in ASan is disabled by default on most ELF targets.
//    See https://sourceware.org/bugzilla/show_bug.cgi?id=19002
/// In LLVM 20, this option is true by default.
//  Commit:
//    https://github.com/llvm/llvm-project/commit/a8d3ae712290d6f85db2deb9164181058f5c1307#diff-6f3485aafcf75a6f836f79dc7ce698a27780a0942b69d68c105154d905e01cb0R244
static cl::opt<bool> ClAsanGlobalsGC(
    "asan-globals-gc",
    cl::desc("Controls whether ASan uses gc-friendly globals instrumentation"),
    cl::Hidden, cl::init(true));

static cl::opt<int>
    ClMsanTrackOrigins("sanitize-memory-track-origins",
                       cl::desc("Simulates -fsanitize-memory-track-origins"),
                       cl::Hidden, cl::init(0));

// The default value of this option is false in LLVM 15, but true in LLVM 23.
static cl::opt<bool>
    ClMsanParamRetval("sanitize-memory-param-retval",
                      cl::desc("Simulates -fsanitize-memory-param-retval"),
                      cl::Hidden,
#if LLVM_VERSION_MAJOR >= 23
                      cl::init(true)
#else
                      cl::init(false)
#endif
    );

static cl::opt<bool>
    ClAsanRecover("sanitize-recover-address",
                  cl::desc("Simulates -fsanitize-recover=address"), cl::Hidden,
                  cl::init(false));

static cl::opt<bool>
    ClMsanRecover("sanitize-recover-memory",
                  cl::desc("Simulates -fsanitize-recover=memory"), cl::Hidden,
                  cl::init(false));

static cl::opt<bool> ClAllRecover("sanitize-recover-all",
                                  cl::desc("Simulates -fsanitize-recover=all"),
                                  cl::Hidden, cl::init(false));

/// There is compile bug for testcase init-order-dlopen.cpp, disable this option
/// for now.
static cl::opt<bool>
    ClAsanPoisonInternalGlobal("xsan-asan-poison-internal-global",
                               cl::desc("Poison internal globals with ASan"),
                               cl::Hidden, cl::init(false));
namespace __xsan {

void PassRegistry::registerPassIntoOpt(StringRef Name, PassAdder AddPass) {
  OptPassAdders.push_back({Name.str(), AddPass});
}

void PassRegistry::registerStartPassIntoClang(PassAdder AddPass) {
  StartPassAdders.push_back(AddPass);
}

void PassRegistry::registerSanitizerPassIntoClang(PassAdder AddPass) {
  HasSanitizerPassAdder = true;
  SanitizerPassAdder = AddPass;
}

void PassRegistry::delegateNonSanPassTo(PassBuilder &PB) {
  if (NonSanPassDelegated)
    return;
  NonSanPassDelegated = true;

  if (!StartPassAdders.empty()) {
    PB.registerPipelineStartEPCallback(
        [this](ModulePassManager &MPM, OptimizationLevel Level) {
          for (PassAdder AddPass : StartPassAdders) {
            if (AddPass)
              AddPass(MPM, Level);
          }
        });
  }

  if (!OptPassAdders.empty()) {
    PB.registerPipelineParsingCallback(
        [this](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
          for (const NamedPassAdder &Entry : OptPassAdders) {
            if (Entry.Name != Name)
              continue;
            if (Entry.AddPass)
              Entry.AddPass(MPM, OptimizationLevel::O0);
            return true;
          }
          return false;
        });
  }
}

class PassRegistry PassRegistry;

bool shouldAsanPoisonInternalGlobals() { return ClAsanPoisonInternalGlobal; }

const AsanOption obtainAsanPassArgs() {
  /*
   The relevant code in clang BackendUtil.cpp::addSanitizer
    ```cpp
    bool UseGlobalGC = asanUseGlobalsGC(TargetTriple, CodeGenOpts);
    bool UseOdrIndicator = CodeGenOpts.SanitizeAddressUseOdrIndicator;
    llvm::AsanDtorKind DestructorKind =
        CodeGenOpts.getSanitizeAddressDtor();
    AddressSanitizerOptions Opts;
    Opts.CompileKernel = CompileKernel;
    Opts.Recover = CodeGenOpts.SanitizeRecover.has(Mask);
    Opts.UseAfterScope = CodeGenOpts.SanitizeAddressUseAfterScope;
    Opts.UseAfterReturn = CodeGenOpts.getSanitizeAddressUseAfterReturn();
    ```
  */
  AsanOption Opts;
  // Here set the default value for each setting, but you can set them by
  // their cl::opt version.
  Opts.Opts.CompileKernel = false;
  Opts.Opts.Recover = ClAsanRecover || ClAllRecover;
  // The default value of UseAfterScope is true
  Opts.Opts.UseAfterScope = ClAsanUseAfterScope;
  Opts.Opts.UseAfterReturn = ClAsanUseAfterReturn;

  Opts.DestructorKind = ClAsanDestructorKind;

  Opts.UseOdrIndicator = ClAsanUseOdrIndicator;
  Opts.UseGlobalGC = ClAsanGlobalsGC;

  return Opts;
}

const MsanOption obtainMsanPassArgs() {
  /*
   The relevant code in clang BackendUtil.cpp::addSanitizer
    ```cpp
    int TrackOrigins = CodeGenOpts.SanitizeMemoryTrackOrigins;
    bool Recover = CodeGenOpts.SanitizeRecover.has(Mask);

    MemorySanitizerOptions options(TrackOrigins, Recover, CompileKernel,
                                    CodeGenOpts.SanitizeMemoryParamRetval);
    ```
  */
  MsanOption Opts(ClMsanTrackOrigins, ClMsanRecover, false, ClMsanParamRetval);

  return Opts;
}

void addAsanToMPM(ModulePassManager &MPM, OptimizationLevel Level) {
  (void)Level;
  if (options::ClDisableAsan)
    return;

  const auto &Opts = obtainAsanPassArgs();
#if LLVM_VERSION_MAJOR >= 23
  MPM.addPass(AddressSanitizerPass(Opts.Opts, Opts.UseGlobalGC,
                                   Opts.UseOdrIndicator, Opts.DestructorKind));
#else
  MPM.addPass(ModuleAddressSanitizerPass(
      Opts.Opts, Opts.UseGlobalGC, Opts.UseOdrIndicator, Opts.DestructorKind));
#endif
}

void addTsanToMPM(ModulePassManager &MPM, OptimizationLevel Level) {
  (void)Level;
  if (options::ClDisableTsan)
    return;
  // Create ctor and init functions.
  MPM.addPass(ModuleThreadSanitizerPass());
  // Function Pass to Module Pass. Instruments functions to detect race
  // conditions reads.
  MPM.addPass(createModuleToFunctionPassAdaptor(ThreadSanitizerPass()));
}

void addMsanToMPM(ModulePassManager &MPM, llvm::OptimizationLevel Level) {
  if (options::ClDisableMsan)
    return;

  const auto &Opts = obtainMsanPassArgs();
#if LLVM_VERSION_MAJOR >= 23
  MPM.addPass(MemorySanitizerPass(Opts.Opts));
  // XSan records sanitizer instrumentation effects only. Post-MSan optimizer
  // rewrites are pure optimization semantics, so keep them out of XSAN_PASS
  // DeSan/ReSan recording.
#ifndef XSAN_PASS
  if (Level != OptimizationLevel::O0) {
    // MemorySanitizer inserts complex instrumentation that mostly follows
    // the logic of the original code, but operates on "shadow" values. It
    // can benefit from re-running some general purpose optimization
    // passes.
    MPM.addPass(RequireAnalysisPass<GlobalsAA, llvm::Module>());
    FunctionPassManager FPM;
    FPM.addPass(EarlyCSEPass(true /* Enable mem-ssa. */));
    FPM.addPass(InstCombinePass());
    FPM.addPass(JumpThreadingPass());
    FPM.addPass(GVNPass());
    FPM.addPass(InstCombinePass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
  }
#endif
#else
  MPM.addPass(ModuleMemorySanitizerPass(Opts.Opts));
  FunctionPassManager FPM;
  FPM.addPass(MemorySanitizerPass(Opts.Opts));
  // XSan records sanitizer instrumentation effects only. Post-MSan optimizer
  // rewrites are pure optimization semantics, so keep them out of XSAN_PASS
  // DeSan/ReSan recording.
#ifndef XSAN_PASS
  if (Level != OptimizationLevel::O0) {
    // MemorySanitizer inserts complex instrumentation that mostly
    // follows the logic of the original code, but operates on
    // "shadow" values. It can benefit from re-running some
    // general purpose optimization passes.
    FPM.addPass(EarlyCSEPass());
    // TODO: Consider add more passes like in
    // addGeneralOptsForMemorySanitizer. EarlyCSEPass makes visible
    // difference on size. It's not clear if the rest is still
    // usefull. InstCombinePass breakes
    // compiler-rt/test/msan/select_origin.cpp.
  }
#endif
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
#endif
}

static void addAsanOptPasses(ModulePassManager &MPM, OptimizationLevel Level) {
  MPM.addPass(AttributeTaggingPass(SanitizerType::ASan));
  addAsanToMPM(MPM, Level);
}

static void addTsanOptPasses(ModulePassManager &MPM, OptimizationLevel Level) {
  MPM.addPass(AttributeTaggingPass(SanitizerType::TSan));
  addTsanToMPM(MPM, Level);
}

static void addMsanOptPasses(ModulePassManager &MPM, OptimizationLevel Level) {
  MPM.addPass(AttributeTaggingPass(SanitizerType::MSan));
  addMsanToMPM(MPM, Level);
}

namespace {

using SanitizerPassAdder = void (*)(llvm::ModulePassManager &,
                                    llvm::OptimizationLevel);

struct SanitizerInstrPlan {
  std::function<bool()> IsEnabled;
  llvm::StringLiteral Name;
  SanitizerPassAdder Run;
};

// The order matters: each enabled sanitizer is added to SanitizingContext in
// this order, and ReSan uses that dynamic order when resolving replay records.
const SanitizerInstrPlan SanitizerInstrPlans[] = {
    {[] { return !options::ClDisableAsan; }, "asan", addAsanToMPM},
    {[] { return !options::ClDisableTsan; }, "tsan", addTsanToMPM},
    {[] { return !options::ClDisableMsan; }, "msan", addMsanToMPM},
};

} // namespace

void SubSanitizers::beginPass(unsigned PassIndex) {
  const bool IsDeSanPass = isDeSanPass(PassIndex);
  if (IsDeSanPass) {
    if (Recording) {
      desan::endRecording();
      Recording = false;
    }
    return;
  }

  if (!Recording && isInSanitizing(PassIndex)) {
    desan::beginRecording();
    Recording = true;
  }
}

bool SubSanitizers::isInSanitizing(unsigned PassIndex) const {
  if (FirstSanitizerPassIndex == -1)
    return false;
  assert(ReSanPassIndex != -1);
  return FirstSanitizerPassIndex <= PassIndex && PassIndex < ReSanPassIndex;
}

void SubSanitizers::resetRunState() { Recording = false; }

void SubSanitizers::addSanitizerWithDeSan(llvm::StringRef SanName,
                                          PassAdder AddSanitizer) {
  desan::requireOrDie(!!State,
                      "sub-sanitizer addSanitizerWithDeSan missing pipeline "
                      "state");
  if (FirstSanitizerPassIndex == -1)
    FirstSanitizerPassIndex = Passes.size();
  AddSanitizer(*this, Level);
  addDeSanPass(SanName);
}

void SubSanitizers::addDeSanPass(llvm::StringRef SanName) {
  desan::requireOrDie(!!State,
                      "sub-sanitizer addDeSanPass missing pipeline state");
  DeSanPassIndices.insert(Passes.size());
  addPass(desan::DeSanPass(SanName, State));
}

void SubSanitizers::addReSanPass() {
  desan::requireOrDie(!!State,
                      "sub-sanitizer addReSanPass missing pipeline state");
  desan::requireOrDie(ReSanPassIndex == -1,
                      "sub-sanitizer addReSanPass called more than once");
  ReSanPassIndex = Passes.size();
  addPass(desan::ReSanPass(State));
}

PreservedAnalyses SubSanitizers::run(Module &IR, ModuleAnalysisManager &AM) {
  __xsan::InterceptionScope Scope(__xsan::desan::InterceptionGroup);
  PreservedAnalyses PA = PreservedAnalyses::all();
  resetRunState();
  desan::requireOrDie(!Recording, "unexpected recording before sanitizing");

  for (unsigned Idx = 0, Size = Passes.size(); Idx != Size; ++Idx) {
    auto *P = Passes[Idx].get();

    PreservedAnalyses PassPA;
    {
      beginPass(Idx);
      TimeTraceScope TimeScope(P->name(), IR.getName());
      PassPA = P->run(IR, AM);
    }

    // // Update the analysis manager as each pass runs and potentially
    // // invalidates analyses.
    // AM.invalidate(IR, PassPA);

    // Finally, intersect the preserved analyses to compute the aggregate
    // preserved set for this pass manager.
    PA.intersect(std::move(PassPA));
  }

  desan::requireOrDie(!Recording, "unexpected recording after sanitizing");

  return PA;
}

SubSanitizers SubSanitizers::loadSubSanitizers(llvm::OptimizationLevel Level) {
  SubSanitizers Sanitizers(Level);
  Sanitizers.State = std::make_shared<desan::SanitizingContext>();

  // PreprocessPass: mark user IR and collect user BBs.
  Sanitizers.addPass(desan::PreprocessPass(Sanitizers.State));

  // Deal with UBSan first, since UBSan is done by clang frontend.
  Sanitizers.addDeSanPass("ubsan");

  for (const auto &[IsEnabled, Name, AddSanitizer] : SanitizerInstrPlans) {
    if (!IsEnabled())
      continue;

    // SanitizerPass: perform sanitizing instrumentation.
    // DeSanPass: restore user IR for this sanitizer.
    Sanitizers.addSanitizerWithDeSan(Name, AddSanitizer);
  }

  // ReSanPass: replay sanitizer events to user IR.
  Sanitizers.addReSanPass();

  if (options::ClDebug)
    Sanitizers.addPass(llvm::VerifierPass());

  return Sanitizers;
}

/// TODO: explore a more efficient optimization pipeline for XSAN.
llvm::ModulePassManager createPostOptimizationPasses(OptimizationLevel Level) {
  ModulePassManager MPM;
  FunctionPassManager FPM;

  // PromotePass: promote alloca/load/store instructions to registers,
  // i.e., -mem2reg
  FPM.addPass(PromotePass());
  // SimplifyCFG: simplify the CFG to improve optimization
  FPM.addPass(SimplifyCFGPass());
  // InstSimplify: simplify instructions
  FPM.addPass(InstSimplifyPass());
  // EarlyCSE: eliminate common subexpressions that are not loop-invariant
  FPM.addPass(EarlyCSEPass());
  FPM.addPass(SimplifyCFGPass());
  // InstCombine: peep-hole optiimization of instructions
  FPM.addPass(InstCombinePass());
  FPM.addPass(SimplifyCFGPass());

  if (Level.getSpeedupLevel() >= 2) {
    // GVN: global value numbering, is a O2 optimization
    // eliminates inter-blocks redundant computations by numbering.
    FPM.addPass(GVNPass());
    FPM.addPass(InstCombinePass());
    FPM.addPass(SimplifyCFGPass());
  }

  /* Loop Optimizations */

  // LoopSimplify: simplify loops
  FPM.addPass(LoopSimplifyPass());
  // LCSSA: convert loops to static single assignment form
  FPM.addPass(LCSSAPass());
  // SCCPPass: detect and optimize single-condition critical sections
  FPM.addPass(SCCPPass());
  // LoopLoadElimination: load elimination in loops
  FPM.addPass(LoopLoadEliminationPass());
  // LoopSink: sinking some inst in loops
  FPM.addPass(LoopSinkPass());

  // // LICM: loop invariant code motion (O3 optimization)
  // LICMPass pas;
  // FPM.addPass(LICMPass());

  /* Inst Combine */
  // InstCombine: combine redundant instructions
  FPM.addPass(InstCombinePass());
  // SimplifyCFG: simplify the CFG after instcombine
  FPM.addPass(SimplifyCFGPass());

  /* Tail Call Elimination */
  /// TODO: figrue out is it OKay?
  FPM.addPass(TailCallElimPass());

  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
  return MPM;
}

void registerAsanForClangAndOpt(PassBuilder &PB) {
  // Clang default pipelines are rewritten after PassBuilder has assembled the
  // original sanitizer pipeline.  Registering this plugin on the same
  // OptimizerLast extension point as Clang's addSanitizers callback would put
  // it before SanCov, because plugin callbacks are registered earlier.  The
  // livepatch MPM rewriter stays sanitizer-agnostic: this registration provides
  // the indirect callback used at Clang's original sanitizer position.
  //
  //     PB.registerOptimizerLastEPCallback(
  // #if LLVM_VERSION_MAJOR >= 22
  //       [=](ModulePassManager &MPM, OptimizationLevel Level,
  //       ThinOrFullLTOPhase) {
  // #else
  //       [=](ModulePassManager &MPM, OptimizationLevel Level) {
  // #endif
  //         addAsanToMPM(MPM, Level);
  //       });
  PassRegistry.registerSanitizerPassIntoClang(addAsanToMPM);
  PassRegistry.registerPassIntoOpt("asan", addAsanOptPasses);
  PassRegistry.delegateNonSanPassTo(PB);
}

void registerTsanForClangAndOpt(PassBuilder &PB) {
  // See registerAsanForClangAndOpt: clang insertion is handled by the final
  // MPM rewriter so SanCov keeps Clang's original position before sanitizers.
  //
  //     PB.registerOptimizerLastEPCallback(
  // #if LLVM_VERSION_MAJOR >= 22
  //       [=](ModulePassManager &MPM, OptimizationLevel Level,
  //       ThinOrFullLTOPhase) {
  // #else
  //       [=](ModulePassManager &MPM, OptimizationLevel Level) {
  // #endif
  //         addTsanToMPM(MPM, Level);
  //       });
  PassRegistry.registerSanitizerPassIntoClang(addTsanToMPM);
  PassRegistry.registerPassIntoOpt("tsan", addTsanOptPasses);
  PassRegistry.delegateNonSanPassTo(PB);
}

void registerMsanForClangAndOpt(PassBuilder &PB) {
  // See registerAsanForClangAndOpt: clang insertion is handled by the final
  // MPM rewriter so SanCov keeps Clang's original position before sanitizers.
  //
  //     PB.registerOptimizerLastEPCallback(
  // #if LLVM_VERSION_MAJOR >= 22
  //       [=](ModulePassManager &MPM, OptimizationLevel Level,
  //       ThinOrFullLTOPhase) {
  // #else
  //       [=](ModulePassManager &MPM, OptimizationLevel Level) {
  // #endif
  //         addMsanToMPM(MPM, Level);
  //       });
  PassRegistry.registerSanitizerPassIntoClang(addMsanToMPM);
  PassRegistry.registerPassIntoOpt("msan", addMsanOptPasses);
  PassRegistry.delegateNonSanPassTo(PB);
}

} // namespace __xsan
