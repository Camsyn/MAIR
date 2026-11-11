#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIRPassTestUtil.h"

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

} // namespace
