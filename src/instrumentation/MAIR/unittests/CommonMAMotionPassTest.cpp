#include "MAIR/Transforms/CommonMAMotionPass.h"
#include "MAIRPassTestUtil.h"

using namespace mair;

namespace {

void placeBranchReadsOnEntries(MAIRGraph &Graph, llvm::Function &F) {
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  Observation *LeftAccess = findMutableMemoryObservation(Graph, "left");
  Observation *RightAccess = findMutableMemoryObservation(Graph, "right");
  ASSERT_NE(LeftAccess, nullptr);
  ASSERT_NE(RightAccess, nullptr);
  Observation &LeftEntry = Graph.getBBEntryRepresentative(Left);
  Observation &RightEntry = Graph.getBBEntryRepresentative(Right);
  LeftEntry.addAccess(*LeftAccess->begin());
  LeftAccess->erase(LeftAccess->begin());
  RightEntry.addAccess(*RightAccess->begin());
  RightAccess->erase(RightAccess->begin());
}

TEST(CommonMAMotionPass, HoistsIdenticalReadsToTheDominatingBlock) {
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
  EXPECT_FALSE(CommonMAMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "left"), 0u);
  EXPECT_EQ(accessCount(Graph, "right"), 0u);
  EXPECT_EQ(accessCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(CommonMAMotionPass, AddressSanitizerDoesNotHoist) {
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
  EXPECT_TRUE(CommonMAMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "left"), 1u);
  EXPECT_EQ(accessCount(Graph, "right"), 1u);
  EXPECT_EQ(accessCount(Graph, "entry"), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(CommonMAMotionPass, LeavesAOneSidedReadInPlace) {
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
  Observation *LeftAccess = findMutableMemoryObservation(Graph, "left");
  ASSERT_NE(LeftAccess, nullptr);
  Observation &LeftEntry = Graph.getBBEntryRepresentative(Left);
  LeftEntry.addAccess(*LeftAccess->begin());
  LeftAccess->erase(LeftAccess->begin());
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(CommonMAMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "left"), 1u);
  EXPECT_EQ(accessCount(Graph, "entry"), 0u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
