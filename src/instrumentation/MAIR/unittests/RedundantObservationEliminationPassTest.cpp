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
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "body"), 1u);
  EXPECT_EQ(observationCount(Graph, "exit"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     RedundantEliminationKeepsDominatingLoopBodySeparateFromExit) {
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
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "loop"), 1u);
  EXPECT_EQ(observationCount(Graph, "exit"), 1u);
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
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "loop"), 1u);
  const ProgramPoint *Kept = nullptr;
  for (const ProgramPoint &PP : testProgramPoints(Graph))
    if (PP.isMemoryInstSite() && PP.getBasicBlock() &&
        PP.getBasicBlock()->getName() == "loop" && !PP.empty())
      Kept = &PP;
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
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "then"), 0u);
  EXPECT_EQ(observationCount(Graph, "exit"), 1u);
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
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "then"), 1u);
  EXPECT_EQ(observationCount(Graph, "exit"), 1u);
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
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  const ProgramPoint *Kept = nullptr;
  for (const ProgramPoint &PP : testProgramPoints(Graph))
    if (!PP.empty())
      Kept = &PP;
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
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  const ProgramPoint *Kept = nullptr;
  for (const ProgramPoint &PP : testProgramPoints(Graph))
    if (!PP.empty())
      Kept = &PP;
  ASSERT_NE(Kept, nullptr);
  EXPECT_EQ(Kept->begin()->getKind(), AccessKind::Read);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     ConditionalWitnessCannotCoverAnUnconditionalObservation) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %take) {
entry:
  br i1 %take, label %access, label %exit
access:
  %conditional = load i32, ptr %p, align 4
  br label %exit
exit:
  %unconditional = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  ProgramPoint *Source = findMutableMemoryProgramPoint(Graph, "access");
  ProgramPoint *Target = findMutableMemoryProgramPoint(Graph, "exit");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(Target, nullptr);
  ProgramPoint &Guarded = Graph.createProgramPoint(
      ProgramPointSite::anchored(Target->getAnchor(),
                                 ProgramPoint::SiteKind::BasicBlockEntry),
      AnyProvenanceExecutedGuard(Source->begin()->getInstructions()));
  Guarded.addObservation(*Source->begin());
  Source->erase(Source->begin());
  Graph.addEdge(Guarded, *Target, PPEdgeKind::Dom);
  ASSERT_TRUE(Graph.verify());
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(
      RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Guarded.size(), 1u);
  EXPECT_EQ(Target->size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
