#include "AttributeTaggingPass.hpp"
#include "Instrumentation.h"
#include "PassRegistry.h"
#include "UbsanInstTagging.hpp"
#include "Utils/Logging.h"
#include "Utils/Options.h"
#include "debug.h"
#include "xsan_common.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;
using namespace __xsan;

namespace __xsan {

static void verifyOriginalPassNotRun(Module &M) {
  static constexpr std::pair<const char *, const char *> KeyFuncNames[] = {
      {"asan.module_ctor", "ASan"},
      {"msan.module_ctor", "MSan"},
      {"tsan.module_ctor", "TSan"},
  };
  for (const auto &[funcName, passName] : KeyFuncNames) {
    auto *Func = M.getFunction(funcName);
    if (!Func)
      continue;
    if (Func->hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
      continue;
    FATAL("%s's original pass has already executed before XSan — this "
          "is unsupported.\n\n" ERR_MSG_4_ORIG_PASS,
          passName);
  }
}

class SanitizerCompositorPass
    : public llvm::PassInfoMixin<SanitizerCompositorPass> {
public:
  SanitizerCompositorPass(OptimizationLevel level);
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
  //   void printPipeline(llvm::raw_ostream &OS,
  //                      llvm::function_ref<llvm::StringRef(llvm::StringRef)>
  //                      MapClassName2PassName);
  static bool isRequired() { return true; }

private:
  OptimizationLevel Level;
};

void registerAnalysisForXsan(PassBuilder &PB) {
  if (!options::ClDisableAsan) {
    registerAnalysisForAsan(PB);
  }
  if (!options::ClDisableTsan) {
    registerAnalysisForTsan(PB);
  }
  if (!options::ClDisableMsan) {
    registerAnalysisForMsan(PB);
  }
}

void registerXsanForClangAndOpt(llvm::PassBuilder &PB) {
  registerAnalysisForXsan(PB);

  // Register on the start of the pipeline.
  PB.registerPipelineStartEPCallback(
      [](ModulePassManager &MPM, OptimizationLevel _) {
        MPM.addPass(UbsanInstTaggingPass());
      });

  // Register on the end of the pipeline.
  PB.registerOptimizerLastEPCallback(
#if LLVM_VERSION_MAJOR >= 22
      [=](ModulePassManager &MPM, OptimizationLevel level, ThinOrFullLTOPhase) {
#else
      [=](ModulePassManager &MPM, OptimizationLevel level) {
#endif
        MPM.addPass(SanitizerCompositorPass(level));
      });

  // 这里注册opt回调的名称
  PB.registerPipelineParsingCallback(
      [=](StringRef Name, ModulePassManager &MPM,
          ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "xsan") {
          MPM.addPass(AttributeTaggingPass(SanitizerType::XSan));
          MPM.addPass(UbsanInstTaggingPass());
          MPM.addPass(SanitizerCompositorPass(OptimizationLevel::O0));
          return true;
        }
        return false;
      });
}

} // namespace __xsan

namespace {
/*
  This visitor traverses the IR before the instrumentation of each
  sub-sanitizer.
  - (Manage MemIntrinsic Uniformly) Replace memintrinsic with __xsan_memset, and
  etc.
*/
struct XSanVisitor : public InstVisitor<XSanVisitor> {

  XSanVisitor(Module &M) {
    initializeType(M);
    initializeCallbacks(M);
  }

  void visitMemSetInst(MemSetInst &I) {
    I.getIntrinsicID();
    IRBuilder<> IRB(&I);
    IRB.CreateCall(MemsetFn,
                   {I.getArgOperand(0), I.getArgOperand(1),
                    IRB.CreateIntCast(I.getArgOperand(2), IntptrTy, false)});
    I.eraseFromParent();
  }
  void visitMemCpyInst(MemCpyInst &I) {
    IRBuilder<> IRB(&I);
    IRB.CreateCall(MemcpyFn,
                   {I.getArgOperand(0), I.getArgOperand(1),
                    IRB.CreateIntCast(I.getArgOperand(2), IntptrTy, false)});
    I.eraseFromParent();
  }
  void visitMemMoveInst(MemMoveInst &I) {
    IRBuilder<> IRB(&I);
    IRB.CreateCall(MemmoveFn,
                   {I.getArgOperand(0), I.getArgOperand(1),
                    IRB.CreateIntCast(I.getArgOperand(2), IntptrTy, false)});
    I.eraseFromParent();
  }

private:
  void initializeType(Module &M) {
    LLVMContext &C = M.getContext();
    IRBuilder<> IRB(C);
    IntptrTy = IRB.getIntPtrTy(M.getDataLayout());
    PtrTy = PointerType::getUnqual(C);
    I32Ty = IRB.getInt32Ty();
  }
  void initializeCallbacks(Module &M) {
    MemmoveFn =
        M.getOrInsertFunction("__xsan_memmove", PtrTy, PtrTy, PtrTy, IntptrTy);
    MemcpyFn =
        M.getOrInsertFunction("__xsan_memcpy", PtrTy, PtrTy, PtrTy, IntptrTy);
    MemsetFn =
        M.getOrInsertFunction("__xsan_memset", PtrTy, PtrTy, I32Ty, IntptrTy);
  }

  /// XSan runtime replacements for memmove, memcpy and memset.
  FunctionCallee MemmoveFn, MemcpyFn, MemsetFn;
  Type *IntptrTy, *I32Ty;
  PointerType *PtrTy;
};
} // namespace

SanitizerCompositorPass::SanitizerCompositorPass(OptimizationLevel level)
    : Level(level) {}

PreservedAnalyses SanitizerCompositorPass::run(Module &M,
                                               ModuleAnalysisManager &MAM) {
  verifyOriginalPassNotRun(M);
  options::ClDebug.setValue(options::ClDebug || !!std::getenv("XSAN_DEBUG"));

  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  XSanVisitor Visitor(M);

  LoopOptLeval level = options::opt::loopOptLevel();
  if (level != LoopOptLeval::NoOpt) {
    for (auto &F : M) {
      if (F.isDeclaration() || F.empty())
        continue;
      LoopMopInstrumenter LoopInstrumenter =
          LoopMopInstrumenter::create(F, FAM, level);
      LoopInstrumenter.instrument();
      Visitor.visit(F);
    }
  }

  SubSanitizers Sanitizers = SubSanitizers::loadSubSanitizers(Level);
  /// Unlike ModulePassManager, SubSanitizers does not invalidate Analysises
  /// between the runnings of sanitizers' passes.
  PreservedAnalyses PA = Sanitizers.run(M, MAM);
  if (Level.getSpeedupLevel() > 0 && options::opt::enablePostOpt()) {
    ModulePassManager PostOpts = createPostOptimizationPasses(Level);
    /// Invalidate Analyses to re-run them with the post-optimization passes.
    MAM.invalidate(M, PA);
    for (auto &F : M) {
      FAM.invalidate(F, PA);
    }
    PostOpts.run(M, MAM);
  }

  Log.displayLogs(M.getName());

  return PA;
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "XSan Composition Pass", LLVM_VERSION_STRING,
          __xsan::registerXsanForClangAndOpt};
}
