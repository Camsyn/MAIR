#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"
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

TEST(RedundantObservationEliminationPass, RetainedMemsetCoversALaterStore) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
define void @f(ptr %p) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 8, i1 false)
  store i32 1, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  const ProgramPoint *Memset = findMutableMemoryProgramPoint(Graph, "entry");
  ASSERT_NE(Memset, nullptr);
  ASSERT_FALSE(Memset->empty());
  EXPECT_TRUE(Memset->begin()->isCallLike());
  EXPECT_FALSE(Memset->begin()->isDeletable());
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
  EXPECT_TRUE(Kept->begin()->isCallLike());
  EXPECT_EQ(Kept->begin()->getKind(), AccessKind::Write);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     RetainedMemsetDoesNotCoverAVolatileStore) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
define void @f(ptr %p) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 8, i1 false)
  store volatile i32 1, ptr %p, align 4
  ret void
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
  EXPECT_EQ(observationCount(Graph, "entry"), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     OrdersObservationsByMemoryObjectCreation) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, ptr %q) {
entry:
  %a = load i32, ptr %q, align 4
  %b = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  MAIRGraphAnalysisManager AM;
  runPassOnGraphTree(SimplifyGraphPass(), Graph, AM);
  runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);

  // The order must follow the first access to each base, not the addresses of
  // the LLVM values, which differ between runs.
  llvm::SmallVector<const llvm::Value *, 2> Bases;
  for (const ProgramPoint &PP : testProgramPoints(Graph))
    for (const Observation &Obs : PP)
      Bases.push_back(Obs.getBase());
  ASSERT_EQ(Bases.size(), 2u);
  EXPECT_EQ(Bases[0], F.getArg(1));
  EXPECT_EQ(Bases[1], F.getArg(0));
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     KeepsAccessCoveredOnlyInALaterIteration) {
  llvm::LLVMContext Ctx;
  // B post-dominates A only around the backedge, where %p is another address.
  auto M = parseModule(R"IR(
define void @f(ptr %base, i64 %n) {
entry:
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %A ]
  %p = getelementptr inbounds i32, ptr %base, i64 %i
  %c = icmp ult i64 %i, 3
  br i1 %c, label %X, label %B
X:
  br label %A
B:
  store i32 1, ptr %p, align 4
  %c2 = icmp eq i64 %i, %n
  br i1 %c2, label %exit, label %A
A:
  store i32 2, ptr %p, align 4
  %i.next = add i64 %i, 1
  br label %H
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F, MAIROptimizationConfig::forThreadSanitizer());

  ASSERT_EQ(Graph.children().size(), 1u);
  MAIRGraph &Body = *Graph.children().front();
  auto Block = [&](llvm::StringRef Name) -> const llvm::BasicBlock & {
    for (const llvm::BasicBlock &BB : F)
      if (BB.getName() == Name)
        return BB;
    llvm_unreachable("missing block");
  };
  // Post-dominance that holds only by starting another iteration is not an
  // edge of the loop body graph.
  EXPECT_FALSE(Body.hasEdge(Body.getBBEntryRepresentative(Block("B")),
                            Body.getBBExitRepresentative(Block("A")),
                            PPEdgeKind::PostDom));
  EXPECT_FALSE(Body.hasEdge(Body.getBBEntryRepresentative(Block("H")),
                            Body.getBBExitRepresentative(Block("A")),
                            PPEdgeKind::PostDom));
  // Post-dominance inside one iteration still is.
  EXPECT_TRUE(Body.hasEdge(Body.getBBEntryRepresentative(Block("A")),
                           Body.getBBExitRepresentative(Block("X")),
                           PPEdgeKind::PostDom));

  MAIRGraphAnalysisManager AM;
  runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
  EXPECT_EQ(observationCount(Graph, "A"), 1u);
  EXPECT_EQ(observationCount(Graph, "B"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     StillEliminatesAccessCoveredLaterInTheSameIteration) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %base, i64 %n, i1 %c) {
entry:
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %L ]
  %p = getelementptr inbounds i32, ptr %base, i64 %i
  br i1 %c, label %X, label %Y
X:
  store i32 1, ptr %p, align 4
  br label %L
Y:
  br label %L
L:
  store i32 2, ptr %p, align 4
  %i.next = add i64 %i, 1
  %more = icmp ult i64 %i.next, %n
  br i1 %more, label %H, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                               MAIROptimizationConfig::forThreadSanitizer());
  MAIRGraphAnalysisManager AM;
  runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
  EXPECT_EQ(observationCount(Graph, "X"), 0u);
  EXPECT_EQ(observationCount(Graph, "L"), 1u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
