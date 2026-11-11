#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIRPassTestUtil.h"

#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Instructions.h"

using namespace mair;

namespace {

llvm::Function &prepare(llvm::LLVMContext &Ctx,
                        std::unique_ptr<llvm::Module> &M,
                        llvm::PassInstrumentationCallbacks &PIC,
                        llvm::FunctionAnalysisManager &FAM) {
  M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %value = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)IR",
                  Ctx);
  EXPECT_TRUE(M);
  registerLoopAnalyses(FAM, PIC);
  return *M->getFunction("f");
}

TEST(LoopPeriodicObservationCombinePass, AggregatesAUnitStrideLoop) {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  llvm::Function &F = prepare(Ctx, M, PIC, FAM);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_FALSE(
      runPassOnGraphTree(LoopPeriodicObservationCombinePass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "exit"), 1u);
  EXPECT_EQ(observationCount(Graph, "header"), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(LoopPeriodicObservationCombinePass,
     AddressSanitizerAggregatesAUnitStrideLoop) {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  llvm::Function &F = prepare(Ctx, M, PIC, FAM);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(
      F, FAM, MAIROptimizationConfig::forAddressSanitizer());
  llvm::Instruction *Load = findMutableMemoryProgramPoint(Graph, "header")
                                ->begin()
                                ->getInstructions()
                                .front();
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_FALSE(
      runPassOnGraphTree(LoopPeriodicObservationCombinePass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "header"), 0u);
  EXPECT_EQ(observationCount(Graph, "exit"), 1u);
  ASSERT_EQ(Graph.children().size(), 1u);
  auto Exits = Graph.children().front()->getLoopExits();
  ASSERT_EQ(Exits.size(), 1u);
  const ProgramPoint &Target = *Exits.front();
  EXPECT_EQ(Target.getParent(), &Graph);
  ASSERT_EQ(Target.size(), 1u);
  EXPECT_TRUE(Target.hasAlwaysGuard());
  const Observation &Combined = *Target.begin();
  EXPECT_EQ(Combined.getInstructions(),
            (llvm::ArrayRef<llvm::Instruction *>{Load}));
  ASSERT_NE(Combined.getContiguousRegion(), nullptr);
  EXPECT_EQ(*Combined.getContiguousRegion(),
            AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                           AbstractValue::constant(4)));
  EXPECT_TRUE(Graph.verify());
}

TEST(LoopPeriodicObservationCombinePass, LeavesAStraightLineLoadInPlace) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %value = load i8, ptr %p, align 1
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(*M->getFunction("f"), FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_TRUE(
      runPassOnGraphTree(LoopPeriodicObservationCombinePass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(LoopPeriodicObservationCombinePass,
     PreservesMixedCandidateOrderWithAndWithoutMerging) {
  for (bool Merge : {true, false}) {
    for (bool SinglePoint : {true, false}) {
      SCOPED_TRACE(Merge);
      SCOPED_TRACE(SinglePoint);
      llvm::LLVMContext Ctx;
      auto M = parseModule(R"IR(
define void @f(ptr %base, ptr %fixed) {
entry:
  br label %header
header:
  %index = phi i64 [ 0, %entry ], [ %next, %header ]
  %varying = getelementptr i8, ptr %base, i64 %index
  %first = load i8, ptr %fixed, align 1
  %second = load i8, ptr %varying, align 1
  %third = load i8, ptr %fixed, align 1
  store i8 0, ptr %varying, align 1
  %fifth = load i8, ptr %varying, align 1
  %sixth = load i8, ptr %fixed, align 1
  %next = add nuw i64 %index, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)IR",
                           Ctx);
      ASSERT_TRUE(M);
      llvm::Function &F = *M->getFunction("f");
      llvm::PassInstrumentationCallbacks PIC;
      llvm::FunctionAnalysisManager FAM;
      registerLoopAnalyses(FAM, PIC);
      MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
      ASSERT_EQ(Graph.children().size(), 1u);
      MAIRGraph &LoopGraph = *Graph.children().front();
      llvm::SmallVector<ProgramPoint *, 6> Sources;
      llvm::SmallVector<llvm::Instruction *, 6> Origins;
      for (ProgramPoint &PP : LoopGraph) {
        if (PP.empty())
          continue;
        ASSERT_EQ(PP.size(), 1u);
        Sources.push_back(&PP);
        Origins.push_back(PP.begin()->getInstructions().front());
      }
      ASSERT_EQ(Sources.size(), 6u);
      if (SinglePoint) {
        for (ProgramPoint *Source : llvm::ArrayRef(Sources).drop_front()) {
          Sources.front()->addObservation(*Source->begin());
          Source->erase(Source->begin());
        }
        ASSERT_EQ(Sources.front()->size(), 6u);
      }
      ASSERT_TRUE(Graph.verify());
      MAIRGraphAnalysisManager AM;
      registerMAIRGraphAnalyses(AM, &PIC, &FAM);
      LoopPeriodicObservationCombinePass Pass(Merge);
      EXPECT_FALSE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());

      llvm::SmallVector<llvm::Instruction *, 3> Remaining;
      for (ProgramPoint &PP : LoopGraph) {
        EXPECT_EQ(PP.getParent(), &LoopGraph);
        EXPECT_TRUE(PP.hasAlwaysGuard());
        for (Observation &Obs : PP) {
          EXPECT_EQ(Obs.getParent(), &PP);
          ASSERT_EQ(Obs.getInstructions().size(), 1u);
          Remaining.push_back(Obs.getInstructions().front());
        }
      }
      EXPECT_EQ(llvm::ArrayRef(Remaining),
                (llvm::ArrayRef<llvm::Instruction *>{Origins[0], Origins[2],
                                                     Origins[5]}));
      ASSERT_EQ(LoopGraph.getLoopExits().size(), 1u);
      const ProgramPoint &Target = *LoopGraph.getLoopExits().front();
      EXPECT_EQ(Target.getParent(), &Graph);
      EXPECT_TRUE(Target.hasAlwaysGuard());
      ASSERT_EQ(Target.size(), Merge ? 2u : 3u);
      unsigned TargetIndex = 0;
      for (const Observation &Obs : Target) {
        EXPECT_EQ(Obs.getParent(), &Target);
        ASSERT_NE(Obs.getContiguousRegion(), nullptr);
        EXPECT_EQ(*Obs.getContiguousRegion(),
                  AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                                 AbstractValue::constant(4)));
        EXPECT_EQ(Obs.getKind(),
                  TargetIndex == 1u ? AccessKind::Write : AccessKind::Read);
        EXPECT_EQ(Obs.getLevel(), TargetIndex == 1u ? 2u : 1u);
        if (Merge && TargetIndex == 0u) {
          EXPECT_EQ(Obs.getInstructions(), (llvm::ArrayRef<llvm::Instruction *>{
                                               Origins[1], Origins[4]}));
        } else {
          llvm::Instruction *Origin =
              TargetIndex == 0u ? Origins[1]
                                : (TargetIndex == 1u ? Origins[3] : Origins[4]);
          EXPECT_EQ(Obs.getInstructions(),
                    (llvm::ArrayRef<llvm::Instruction *>{Origin}));
        }
        ++TargetIndex;
      }
      EXPECT_TRUE(Graph.verify());
      EXPECT_TRUE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());
      EXPECT_TRUE(Graph.verify());
    }
  }
}

TEST(LoopPeriodicObservationCombinePass,
     ProvenanceFallbackAcceptsOnlyLoadsAndStores) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @probe(ptr) memory(none)

define void @f(ptr %p) {
entry:
  br label %header
header:
  %index = phi i64 [ 0, %entry ], [ %next, %header ]
  %pointer = getelementptr i8, ptr %p, i64 %index
  %derived = getelementptr i8, ptr %pointer, i64 0
  %value = load i8, ptr %pointer, align 1
  store i8 0, ptr %pointer, align 1
  call void @probe(ptr %pointer)
  %next = add nuw i64 %index, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  ASSERT_EQ(Graph.children().size(), 1u);
  MAIRGraph &LoopGraph = *Graph.children().front();
  llvm::Instruction *Load = nullptr;
  llvm::Instruction *Store = nullptr;
  llvm::Instruction *GEP = nullptr;
  llvm::Instruction *Call = nullptr;
  for (llvm::BasicBlock &BB : F) {
    for (llvm::Instruction &Insn : BB) {
      if (llvm::isa<llvm::LoadInst>(Insn))
        Load = &Insn;
      if (llvm::isa<llvm::StoreInst>(Insn))
        Store = &Insn;
      if (Insn.getName() == "derived")
        GEP = &Insn;
      if (llvm::isa<llvm::CallInst>(Insn))
        Call = &Insn;
    }
  }
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  ASSERT_NE(GEP, nullptr);
  ASSERT_NE(Call, nullptr);
  ProgramPoint *Source = findMutableMemoryProgramPoint(Graph, "header");
  ASSERT_NE(Source, nullptr);
  for (ProgramPoint &PP : LoopGraph)
    while (!PP.empty())
      PP.erase(PP.begin());
  AbstractAccess Region(F.getArg(0), AbstractValue::constant(0),
                        AbstractValue::constant(1));
  for (llvm::Instruction *Origin : {GEP, Load, Call, Store}) {
    Observation &Obs = Source->addObservation(
        Observation({Origin}, Region,
                    Origin == Store ? AccessKind::Write : AccessKind::Read));
    EXPECT_TRUE(Obs.isMovable());
  }
  ASSERT_TRUE(Graph.verify());
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  llvm::ScalarEvolution &SE = AM.getResult<LoopAnalysis>(LoopGraph).getSE();
  EXPECT_FALSE(llvm::isa<llvm::SCEVAddRecExpr>(SE.getSCEV(F.getArg(0))));
  const auto *Recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(
      SE.getSCEV(llvm::getLoadStorePointerOperand(Load)));
  ASSERT_NE(Recurrence, nullptr);
  EXPECT_EQ(Recurrence->getLoop(), LoopGraph.getLoop());
  EXPECT_EQ(llvm::getLoadStorePointerOperand(Store),
            llvm::getLoadStorePointerOperand(Load));
  EXPECT_EQ(llvm::getPointerOperand(GEP),
            llvm::getLoadStorePointerOperand(Load));

  LoopPeriodicObservationCombinePass Pass;
  EXPECT_FALSE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());
  ASSERT_EQ(Source->size(), 2u);
  EXPECT_EQ(Source->begin()->getInstructions(),
            (llvm::ArrayRef<llvm::Instruction *>{GEP}));
  EXPECT_EQ(std::next(Source->begin())->getInstructions(),
            (llvm::ArrayRef<llvm::Instruction *>{Call}));
  ASSERT_EQ(LoopGraph.getLoopExits().size(), 1u);
  const ProgramPoint &Target = *LoopGraph.getLoopExits().front();
  EXPECT_EQ(Target.getParent(), &Graph);
  EXPECT_TRUE(Target.hasAlwaysGuard());
  ASSERT_EQ(Target.size(), 2u);
  unsigned TargetIndex = 0;
  for (const Observation &Obs : Target) {
    EXPECT_EQ(Obs.getParent(), &Target);
    EXPECT_EQ(Obs.getInstructions(), (llvm::ArrayRef<llvm::Instruction *>{
                                         TargetIndex == 0u ? Load : Store}));
    ASSERT_NE(Obs.getContiguousRegion(), nullptr);
    EXPECT_EQ(*Obs.getContiguousRegion(),
              AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                             AbstractValue::constant(4)));
    ++TargetIndex;
  }
  EXPECT_TRUE(Graph.verify());
  EXPECT_TRUE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());
  EXPECT_TRUE(Graph.verify());
}

} // namespace
