#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIRPassTestUtil.h"

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
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_FALSE(
      LoopInvariantObservationMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_EQ(observationCount(Graph, "header"), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(LoopInvariantObservationMotionPass, AddressSanitizerDoesNotHoist) {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  llvm::Function &F = prepare(Ctx, M, PIC, FAM);
  MAIRGraph Graph =
      MAIRGraph::build(F, FAM, MAIROptimizationConfig::forAddressSanitizer());
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_TRUE(
      LoopInvariantObservationMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 0u);
  EXPECT_EQ(observationCount(Graph, "header"), 1u);
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
  MAIRGraph Graph = MAIRGraph::build(*M->getFunction("f"), FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_TRUE(
      LoopInvariantObservationMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
