#pragma once

#include "MAIR/MAIR.h"
#include "MAIR/MAIRPasses.h"
#include "MAIRHierarchyTestUtil.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

inline std::unique_ptr<llvm::Module> parseModule(llvm::StringRef IR,
                                                 llvm::LLVMContext &Ctx) {
  llvm::SMDiagnostic Err;
  return llvm::parseAssemblyString(IR, Err, Ctx);
}

inline mair::MAIRGraph buildGraph(llvm::Function &F,
                                  mair::MAIROptimizationConfig Config =
                                      mair::MAIROptimizationConfig::generic()) {
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  FAM.registerPass([] { return llvm::AssumptionAnalysis(); });
  FAM.registerPass([] { return llvm::TargetIRAnalysis(); });
  mair::normalizeMAIRLoops(F, FAM);
  llvm::DominatorTree DT(F);
  llvm::PostDominatorTree PDT(F);
  llvm::LoopInfo LI(DT);
  return mair::MAIRGraph::build(F, std::move(DT), std::move(PDT), std::move(LI),
                                Config);
}

inline unsigned observationCount(const mair::MAIRGraph &Graph,
                                 llvm::StringRef Block) {
  unsigned Count = 0;
  for (const mair::ProgramPoint &PP : testProgramPoints(Graph))
    if (PP.getBasicBlock() && PP.getBasicBlock()->getName() == Block)
      Count += PP.size();
  return Count;
}

inline mair::ProgramPoint *
findMutableMemoryProgramPoint(mair::MAIRGraph &Graph, llvm::StringRef Block) {
  for (mair::ProgramPoint &PP : testProgramPoints(Graph))
    if (PP.isMemoryInstSite() && PP.getBasicBlock() &&
        PP.getBasicBlock()->getName() == Block)
      return &PP;
  return nullptr;
}

inline void registerLoopAnalyses(llvm::FunctionAnalysisManager &FAM,
                                 llvm::PassInstrumentationCallbacks &PIC) {
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  FAM.registerPass([] { return llvm::TargetLibraryAnalysis(); });
  FAM.registerPass([] { return llvm::TargetIRAnalysis(); });
  FAM.registerPass([] { return llvm::AssumptionAnalysis(); });
  FAM.registerPass([] { return llvm::ScalarEvolutionAnalysis(); });
}
