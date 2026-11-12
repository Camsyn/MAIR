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
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

#include <tuple>

inline std::unique_ptr<llvm::Module> parseModule(llvm::StringRef IR,
                                                 llvm::LLVMContext &Ctx) {
  llvm::SMDiagnostic Err;
  return llvm::parseAssemblyString(IR, Err, Ctx);
}

/// Build a graph that owns its analyses, after normalizing the loops of \p F.
template <class BarrierTrait = mair::DefaultBarrierTrait>
mair::MAIRGraph buildGraph(llvm::Function &F,
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
  return mair::MAIRGraph::build<BarrierTrait>(F, std::move(DT), std::move(PDT),
                                              std::move(LI), Config);
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

inline void
expectMergeRejectedWithoutMutation(mair::Observation &Left,
                                   mair::Observation &Right,
                                   llvm::ScalarEvolution *SE = nullptr) {
  auto Snapshot = [](const mair::Observation &Obs) {
    return std::make_tuple(
        mair::MemoryRegion(Obs.getRegion()),
        llvm::SmallVector<llvm::Instruction *, 4>(Obs.getInstructions().begin(),
                                                  Obs.getInstructions().end()),
        Obs.getKind(), Obs.getProperties(), Obs.isMergeable(), Obs.isMovable(),
        Obs.isDeletable(), Obs.getLevel(), Obs.getParent(),
        Obs.getBaseUse().get());
  };
  const auto LeftBefore = Snapshot(Left);
  const auto RightBefore = Snapshot(Right);
  EXPECT_FALSE(Left.mergeWith(Right, SE));
  EXPECT_EQ(Snapshot(Left), LeftBefore);
  EXPECT_EQ(Snapshot(Right), RightBefore);
}

inline bool hasSuccessor(const mair::MAIRGraph &Graph,
                         const mair::ProgramPoint &From,
                         const mair::ProgramPoint &To) {
  (void)Graph;
  for (const mair::ProgramPointEdge &Edge : From.children())
    if (&Edge.getNode() == &To)
      return true;
  return false;
}

inline bool hasSuccessor(const mair::MAIRGraph &Graph,
                         const mair::ProgramPoint &From,
                         const mair::ProgramPoint &To, mair::PPEdgeKind Kind) {
  return Graph.hasEdge(From, To, Kind);
}

inline const mair::ProgramPoint *findBlockEntry(const mair::MAIRGraph &Graph,
                                                llvm::StringRef Name) {
  for (const mair::ProgramPoint &PP : testProgramPoints(Graph))
    if (const auto *Boundary = llvm::dyn_cast<mair::LoopProgramPoint>(&PP))
      if (Boundary->isLoopExit() && PP.getBasicBlock()->getName() == Name)
        return &PP;
  for (const mair::ProgramPoint &PP : testProgramPoints(Graph))
    if (PP.isBasicBlockEntrySite() && PP.getBasicBlock() &&
        PP.getBasicBlock()->getName() == Name)
      return &PP;
  return nullptr;
}

inline const mair::ProgramPoint *findBlockExit(const mair::MAIRGraph &Graph,
                                               llvm::StringRef Name) {
  for (const mair::ProgramPoint &PP : testProgramPoints(Graph))
    if (const auto *Boundary = llvm::dyn_cast<mair::LoopProgramPoint>(&PP))
      if (Boundary->isLoopPreheader() && PP.getBasicBlock()->getName() == Name)
        return &PP;
  for (const mair::ProgramPoint &PP : testProgramPoints(Graph))
    if (PP.isBasicBlockExitSite() && PP.getBasicBlock() &&
        PP.getBasicBlock()->getName() == Name)
      return &PP;
  return nullptr;
}

inline const mair::ProgramPoint *
findMemoryProgramPoint(const mair::MAIRGraph &Graph,
                       llvm::StringRef BlockName) {
  for (const mair::ProgramPoint &PP : testProgramPoints(Graph))
    if (PP.isMemoryInstSite() && PP.getBasicBlock() &&
        PP.getBasicBlock()->getName() == BlockName)
      return &PP;
  return nullptr;
}

struct CallBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::CallBase>(I) && !llvm::isa<llvm::AnyMemIntrinsic>(I);
  }

  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return isForwardBarrier(I);
  }
};

struct ForwardCallBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::CallBase>(I);
  }

  static bool isBackwardBarrier(const llvm::Instruction &) { return false; }
};

struct ForwardReleaseBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
    return Store && Store->isAtomic() &&
           llvm::isReleaseOrStronger(Store->getOrdering());
  }

  static bool isBackwardBarrier(const llvm::Instruction &) { return false; }
};

struct BackwardCallBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &) { return false; }

  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::CallBase>(I);
  }
};
