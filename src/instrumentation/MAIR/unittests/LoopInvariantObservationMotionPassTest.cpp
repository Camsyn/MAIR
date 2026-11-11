#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Materialization/MAIRMaterialization.h"
#include "MAIRPassTestUtil.h"

#include "llvm/IR/Verifier.h"

using namespace mair;

namespace {

llvm::Function &prepare(llvm::LLVMContext &Ctx,
                        std::unique_ptr<llvm::Module> &M,
                        llvm::PassInstrumentationCallbacks &PIC,
                        llvm::FunctionAnalysisManager &FAM) {
  M = parseModule(R"IR(
define void @f(ptr %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %header ]
  %value = load i32, ptr %p, align 4
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit
exit:
  ret void
}
)IR",
                  Ctx);
  EXPECT_TRUE(M);
  registerLoopAnalyses(FAM, PIC);
  return *M->getFunction("f");
}

TEST(LoopInvariantObservationMotionPass, HoistsAnInvariantLoad) {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  llvm::Function &F = prepare(Ctx, M, PIC, FAM);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_FALSE(
      runPassOnGraphTree(LoopInvariantObservationMotionPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_EQ(observationCount(Graph, "header"), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(LoopInvariantObservationMotionPass,
     AddressSanitizerHoistsAnInvariantLoad) {
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
      runPassOnGraphTree(LoopInvariantObservationMotionPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_EQ(observationCount(Graph, "header"), 0u);
  ASSERT_EQ(Graph.children().size(), 1u);
  const ProgramPoint *Target = Graph.children().front()->getLoopPreheader();
  ASSERT_NE(Target, nullptr);
  EXPECT_EQ(Target->getParent(), &Graph);
  ASSERT_EQ(Target->size(), 1u);
  EXPECT_TRUE(Target->hasAlwaysGuard());
  const Observation &Moved = *Target->begin();
  EXPECT_EQ(Moved.getInstructions(),
            (llvm::ArrayRef<llvm::Instruction *>{Load}));
  ASSERT_NE(Moved.getContiguousRegion(), nullptr);
  EXPECT_EQ(*Moved.getContiguousRegion(),
            AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                           AbstractValue::constant(4)));
  EXPECT_TRUE(Graph.verify());
}

TEST(LoopInvariantObservationMotionPass, LeavesAStraightLineLoadInPlace) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %value = load i32, ptr %p, align 4
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
      runPassOnGraphTree(LoopInvariantObservationMotionPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPipeline, EliminatesRepeatedConditionalStoresBeforeMotion) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %n, i1 %cond) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  br i1 %cond, label %store, label %latch
store:
  store i32 1, ptr %p
  store i32 1, ptr %p
  br label %latch
latch:
  %next = add i32 %i, 1
  br label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &Func = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(
      Func, FAM, MAIROptimizationConfig::forAddressSanitizer());
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  MAIRGraphPassManager PM;
  buildMAIRGraphO1Pipeline(PM);
  PM.run(Graph, AM);
  ASSERT_TRUE(Graph.verify());
  unsigned Observations = 0;
  for (ProgramPoint *Point : Graph.allProgramPoints())
    Observations += Point->size();
  ASSERT_EQ(Observations, 1u);
  EXPECT_EQ(observationCount(Graph, "exit"), 1u);

  class Backend final : public MAIRObservationBackend {
  public:
    unsigned Emissions = 0;

    llvm::Error prepare(const MAIRMaterializationPlan &Plan) override {
      EXPECT_EQ(Plan.units().size(), 1u);
      return llvm::Error::success();
    }

    void emit(const MaterializedProgramPoint &Point,
              llvm::IRBuilderBase &) override {
      EXPECT_TRUE(Point.Guarded);
      ASSERT_EQ(Point.Observations.size(), 1u);
      EXPECT_EQ(Point.Observations.front().Source->getInstructions().size(),
                1u);
      ++Emissions;
    }
  } Recorder;
  auto Result = Graph.instantiate(FAM, Recorder);
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return;
  }
  EXPECT_EQ(Result->GuardBranches, 1u);
  EXPECT_EQ(Recorder.Emissions, 1u);
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
}

TEST(LoopInvariantObservationMotionPass,
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
  %first = load i8, ptr %varying, align 1
  %second = load i8, ptr %fixed, align 1
  %third = load i8, ptr %varying, align 1
  %fourth = load i8, ptr %fixed, align 1
  %fifth = load i8, ptr %fixed, align 1
  %sixth = load i8, ptr %varying, align 1
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
      LoopInvariantObservationMotionPass Pass(Merge);
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
      const ProgramPoint *Target = LoopGraph.getLoopPreheader();
      ASSERT_NE(Target, nullptr);
      EXPECT_EQ(Target->getParent(), &Graph);
      EXPECT_TRUE(Target->hasAlwaysGuard());
      ASSERT_EQ(Target->size(), Merge ? 1u : 3u);
      llvm::SmallVector<llvm::Instruction *, 3> Moved;
      for (const Observation &Obs : *Target) {
        EXPECT_EQ(Obs.getParent(), Target);
        ASSERT_NE(Obs.getContiguousRegion(), nullptr);
        EXPECT_EQ(*Obs.getContiguousRegion(),
                  AbstractAccess(F.getArg(1), AbstractValue::constant(0),
                                 AbstractValue::constant(1)));
        EXPECT_EQ(Obs.getInstructions().size(), Merge ? 3u : 1u);
        Moved.append(Obs.getInstructions().begin(),
                     Obs.getInstructions().end());
      }
      EXPECT_EQ(llvm::ArrayRef(Moved),
                (llvm::ArrayRef<llvm::Instruction *>{Origins[1], Origins[3],
                                                     Origins[4]}));
      EXPECT_TRUE(Graph.verify());
      EXPECT_TRUE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());
      EXPECT_TRUE(Graph.verify());
    }
  }
}

TEST(LoopInvariantObservationMotionPass,
     ReusesLiveGuardedExitWithMatchingProvenanceAndReset) {
  for (bool Merge : {true, false}) {
    SCOPED_TRACE(Merge);
    llvm::LLVMContext Ctx;
    auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %n, i1 %cond) {
entry:
  br label %header
header:
  %index = phi i32 [ 0, %entry ], [ %next, %latch ]
  %more = icmp slt i32 %index, %n
  br i1 %more, label %body, label %exit
body:
  br i1 %cond, label %then, label %latch
then:
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %p, align 4
  %third = load i32, ptr %p, align 4
  br label %latch
latch:
  %next = add i32 %index, 1
  br label %header
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
    llvm::SmallVector<ProgramPoint *, 3> Sources;
    llvm::SmallVector<llvm::Instruction *, 3> Origins;
    for (ProgramPoint &PP : LoopGraph) {
      if (PP.empty())
        continue;
      Sources.push_back(&PP);
      Origins.push_back(PP.begin()->getInstructions().front());
    }
    ASSERT_EQ(Sources.size(), 3u);
    AbstractAccess Region(F.getArg(0), AbstractValue::constant(0),
                          AbstractValue::constant(4));
    Sources[0]->erase(Sources[0]->begin());
    Sources[0]->addObservation(Observation({Origins[0], Origins[1], Origins[0]},
                                           Region, AccessKind::Read));
    Sources[1]->erase(Sources[1]->begin());
    Sources[1]->addObservation(Observation({Origins[1], Origins[0], Origins[1]},
                                           Region, AccessKind::Read));
    ASSERT_TRUE(Graph.verify());
    MAIRGraphAnalysisManager AM;
    registerMAIRGraphAnalyses(AM, &PIC, &FAM);
    LoopInvariantObservationMotionPass Pass(Merge);
    EXPECT_FALSE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());

    for (ProgramPoint *Source : Sources)
      EXPECT_TRUE(Source->empty());
    ASSERT_NE(LoopGraph.getLoopPreheader(), nullptr);
    EXPECT_TRUE(LoopGraph.getLoopPreheader()->empty());
    ASSERT_EQ(LoopGraph.getLoopExits().size(), 1u);
    const ProgramPoint *Exit = LoopGraph.getLoopExits().front();
    EXPECT_TRUE(Exit->empty());
    unsigned GuardedTargets = 0;
    for (const ProgramPoint &Target : Graph) {
      if (!Target.hasExecutionGuard())
        continue;
      ++GuardedTargets;
      EXPECT_EQ(Target.getParent(), &Graph);
      EXPECT_EQ(Target.getSite(), Exit->getSite());
      const auto &Guard =
          std::get<AnyProvenanceExecutedGuard>(Target.getGuard());
      EXPECT_EQ(Guard.getResetBlock(), &F.getEntryBlock());
      bool Shared = Guard.getProvenance().size() == 2u;
      EXPECT_EQ(
          Guard.getProvenance(),
          Shared ? (llvm::ArrayRef<llvm::Instruction *>{Origins[0], Origins[1]})
                 : (llvm::ArrayRef<llvm::Instruction *>{Origins[2]}));
      ASSERT_EQ(Target.size(), Shared && !Merge ? 2u : 1u);
      llvm::SmallVector<llvm::Instruction *, 6> Moved;
      for (const Observation &Obs : Target) {
        EXPECT_EQ(Obs.getParent(), &Target);
        ASSERT_NE(Obs.getContiguousRegion(), nullptr);
        EXPECT_EQ(*Obs.getContiguousRegion(), Region);
        EXPECT_EQ(Obs.getInstructions().size(),
                  Shared ? (Merge ? 6u : 3u) : 1u);
        Moved.append(Obs.getInstructions().begin(),
                     Obs.getInstructions().end());
      }
      EXPECT_EQ(llvm::ArrayRef(Moved),
                Shared ? (llvm::ArrayRef<llvm::Instruction *>{
                             Origins[0], Origins[1], Origins[0], Origins[1],
                             Origins[0], Origins[1]})
                       : (llvm::ArrayRef<llvm::Instruction *>{Origins[2]}));
    }
    EXPECT_EQ(GuardedTargets, 2u);
    EXPECT_TRUE(Graph.verify());
    EXPECT_TRUE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());
    EXPECT_TRUE(Graph.verify());
  }
}

TEST(LoopInvariantObservationMotionPass,
     DoesNotHoistAnAccessTheLoopCanExitWithout) {
  llvm::LLVMContext Ctx;
  // The store post-dominates the inner preheader only around the outer
  // backedge: for %j <= 5 the inner loop exits without executing it.
  auto M = parseModule(R"IR(
define void @f(ptr %base, i64 %n) {
entry:
  br label %Ho
Ho:
  %j = phi i64 [ 0, %entry ], [ %j.next, %Lo.latch ]
  %p = getelementptr inbounds i32, ptr %base, i64 %j
  br label %Hi
Hi:
  %i = phi i64 [ 0, %Ho ], [ %i.next, %Hi.latch ]
  %c = icmp ugt i64 %j, 5
  br i1 %c, label %S, label %Lo.latch
S:
  store i32 0, ptr %p, align 4
  %c2 = icmp eq i64 %j, %n
  br i1 %c2, label %exit, label %Hi.latch
Hi.latch:
  %i.next = add i64 %i, 1
  br label %Hi
Lo.latch:
  %j.next = add i64 %j, 1
  br label %Ho
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_TRUE(
      runPassOnGraphTree(LoopInvariantObservationMotionPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "S"), 1u);
  EXPECT_EQ(observationCount(Graph, "Ho"), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(LoopInvariantObservationMotionPass, HoistsAnAccessEveryExitPathExecutes) {
  llvm::LLVMContext Ctx;
  // The store runs only from the fourth iteration on, but the loop cannot be
  // left before it has run.
  auto M = parseModule(R"IR(
define void @f(ptr %p, i64 %n) {
entry:
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %L ]
  %late = icmp ugt i64 %i, 3
  br i1 %late, label %S, label %L
S:
  store i32 0, ptr %p, align 4
  %done = icmp eq i64 %i, %n
  br i1 %done, label %exit, label %L
L:
  %i.next = add i64 %i, 1
  br label %H
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_FALSE(
      runPassOnGraphTree(LoopInvariantObservationMotionPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "S"), 0u);
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
