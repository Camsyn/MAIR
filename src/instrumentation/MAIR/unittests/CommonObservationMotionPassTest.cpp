#include "MAIR/Transforms/CommonObservationMotionPass.h"
#include "MAIRPassTestUtil.h"

using namespace mair;

namespace {

void placeBranchReadsOnEntries(MAIRGraph &Graph, llvm::Function &F) {
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  ProgramPoint *LeftAccess = findMutableMemoryProgramPoint(Graph, "left");
  ProgramPoint *RightAccess = findMutableMemoryProgramPoint(Graph, "right");
  ASSERT_NE(LeftAccess, nullptr);
  ASSERT_NE(RightAccess, nullptr);
  ProgramPoint &LeftEntry = Graph.getBBEntryRepresentative(Left);
  ProgramPoint &RightEntry = Graph.getBBEntryRepresentative(Right);
  LeftEntry.addObservation(*LeftAccess->begin());
  LeftAccess->erase(LeftAccess->begin());
  RightEntry.addObservation(*RightAccess->begin());
  RightAccess->erase(RightAccess->begin());
}

TEST(CommonObservationMotionPass, HoistsIdenticalReadsToTheDominatingBlock) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  %a = load i32, ptr %p, align 4
  br label %exit
right:
  %b = load i32, ptr %p, align 4
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  placeBranchReadsOnEntries(Graph, F);
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(CommonObservationMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "left"), 0u);
  EXPECT_EQ(observationCount(Graph, "right"), 0u);
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(CommonObservationMotionPass, AddressSanitizerDoesNotHoist) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  %a = load i32, ptr %p, align 4
  br label %exit
right:
  %b = load i32, ptr %p, align 4
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph =
      buildGraph(F, MAIROptimizationConfig::forAddressSanitizer());
  placeBranchReadsOnEntries(Graph, F);
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(CommonObservationMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "left"), 1u);
  EXPECT_EQ(observationCount(Graph, "right"), 1u);
  EXPECT_EQ(observationCount(Graph, "entry"), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(CommonObservationMotionPass, LeavesAOneSidedReadInPlace) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  %a = load i32, ptr %p, align 4
  br label %exit
right:
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  llvm::BasicBlock &Left = *std::next(F.begin());
  ProgramPoint *LeftAccess = findMutableMemoryProgramPoint(Graph, "left");
  ASSERT_NE(LeftAccess, nullptr);
  ProgramPoint &LeftEntry = Graph.getBBEntryRepresentative(Left);
  LeftEntry.addObservation(*LeftAccess->begin());
  LeftAccess->erase(LeftAccess->begin());
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(CommonObservationMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "left"), 1u);
  EXPECT_EQ(observationCount(Graph, "entry"), 0u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
