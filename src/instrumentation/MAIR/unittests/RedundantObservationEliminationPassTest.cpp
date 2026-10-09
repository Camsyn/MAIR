#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIRPassTestUtil.h"

using namespace mair;

namespace {

TEST(RedundantObservationEliminationPass,
     RedundantEliminationKeepsNondominatingLoopExitRead) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define i32 @f(ptr %base, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %p = getelementptr i32, ptr %base, i64 %i
  %selected = icmp eq i64 %i, 0
  br i1 %selected, label %body, label %latch
body:
  %v = load i32, ptr %p, align 4
  br label %latch
latch:
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, %n
  br i1 %more, label %loop, label %exit
exit:
  %last = load i32, ptr %p, align 4
  ret i32 %last
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "body"), 1u);
  EXPECT_EQ(accessCount(Graph, "exit"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     RedundantEliminationDomStillCoversDominatingLoopExit) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define i32 @f(ptr %base, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr i32, ptr %base, i64 %i
  %v = load i32, ptr %p, align 4
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, %n
  br i1 %more, label %loop, label %exit
exit:
  %last = load i32, ptr %p, align 4
  ret i32 %last
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  // The loop block dominates the exit, so Dom coverage still removes the exit
  // read. The same-loop guard applies only to PostDom edges.
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "loop"), 1u);
  EXPECT_EQ(accessCount(Graph, "exit"), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     RedundantEliminationDeletesEarlierSameLoopRead) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define i32 @f(ptr %base, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr i32, ptr %base, i64 %i
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %p, align 4
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, %n
  br i1 %more, label %loop, label %exit
exit:
  ret i32 %first
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "loop"), 1u);
  const Observation *Kept = nullptr;
  for (const Observation &Obs : Graph)
    if (Obs.isMemoryInstSite() && Obs.getBasicBlock() &&
        Obs.getBasicBlock()->getName() == "loop" && !Obs.empty())
      Kept = &Obs;
  ASSERT_NE(Kept, nullptr);
  EXPECT_EQ(Kept->begin()->getInstructions().front()->getName(), "second");
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     ThreadSanitizerDeletesPostDominatedRead) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define i32 @f(ptr %p, i1 %take) {
entry:
  br i1 %take, label %then, label %exit
then:
  %earlier = load i32, ptr %p, align 4
  br label %exit
exit:
  %later = load i32, ptr %p, align 4
  ret i32 %later
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "then"), 0u);
  EXPECT_EQ(accessCount(Graph, "exit"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     AddressSanitizerKeepsPostDominatedRead) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define i32 @f(ptr %p, i1 %take) {
entry:
  br i1 %take, label %then, label %exit
then:
  %earlier = load i32, ptr %p, align 4
  br label %exit
exit:
  %later = load i32, ptr %p, align 4
  ret i32 %later
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forAddressSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "then"), 1u);
  EXPECT_EQ(accessCount(Graph, "exit"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     ThreadSanitizerWriteCoversEarlierRead) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %value = load i32, ptr %p, align 4
  store i32 1, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "entry"), 1u);
  const Observation *Kept = nullptr;
  for (const Observation &Obs : Graph)
    if (!Obs.empty())
      Kept = &Obs;
  ASSERT_NE(Kept, nullptr);
  EXPECT_EQ(Kept->begin()->getKind(), AccessKind::Write);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     AddressSanitizerReadCoversLaterWrite) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %value = load i32, ptr %p, align 4
  store i32 1, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forAddressSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(accessCount(Graph, "entry"), 1u);
  const Observation *Kept = nullptr;
  for (const Observation &Obs : Graph)
    if (!Obs.empty())
      Kept = &Obs;
  ASSERT_NE(Kept, nullptr);
  EXPECT_EQ(Kept->begin()->getKind(), AccessKind::Read);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
