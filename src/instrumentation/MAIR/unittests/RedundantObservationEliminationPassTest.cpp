#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIRPassTestUtil.h"

#include "llvm/IR/Instructions.h"

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
     AddressSanitizerEliminatesPostDominatedRead) {
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
  // ASan checks may already land after the access they guard, so a later
  // check that is certain to run covers an earlier one, as it does for TSan.
  EXPECT_FALSE(
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "then"), 0u);
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
     AddressSanitizerReadAndWriteCoverEachOther) {
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
  // ASan does not tell reads from writes, so each covers the other. The
  // earlier one is examined first and goes.
  EXPECT_EQ(Kept->begin()->getKind(), AccessKind::Write);
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
  // Post-dominance that holds only by starting another iteration names the
  // loop it iterates: H post-dominates A through the backedge.
  const ProgramPointEdge *AroundBackedge =
      Body.getBBEntryRepresentative(Block("H"))
          .findChild(Body.getBBExitRepresentative(Block("A")),
                     PPEdgeKind::PostDom);
  ASSERT_NE(AroundBackedge, nullptr);
  ASSERT_NE(AroundBackedge->getIteratedCycle(), nullptr);
  EXPECT_EQ(AroundBackedge->getIteratedCycle()->getHeader(), &Block("H"));
  // Post-dominance inside one iteration does not.
  const ProgramPointEdge *WithinIteration =
      Body.getBBEntryRepresentative(Block("A"))
          .findChild(Body.getBBExitRepresentative(Block("X")),
                     PPEdgeKind::PostDom);
  ASSERT_NE(WithinIteration, nullptr);
  EXPECT_EQ(WithinIteration->getIteratedCycle(), nullptr);

  MAIRGraphAnalysisManager AM;
  runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
  EXPECT_EQ(observationCount(Graph, "A"), 1u);
  EXPECT_EQ(observationCount(Graph, "B"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(RedundantObservationEliminationPass,
     EliminatesInvariantAccessCoveredInALaterIteration) {
  llvm::LLVMContext Ctx;
  // The loop of KeepsAccessCoveredOnlyInALaterIteration with a fixed address:
  // B covers A although it runs an iteration later, because %base is the
  // same bytes in every iteration.
  auto M = parseModule(R"IR(
define void @f(ptr %base, i64 %n) {
entry:
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %A ]
  %c = icmp ult i64 %i, 3
  br i1 %c, label %X, label %B
X:
  br label %A
B:
  store i32 1, ptr %base, align 4
  %c2 = icmp eq i64 %i, %n
  br i1 %c2, label %exit, label %A
A:
  store i32 2, ptr %base, align 4
  %i.next = add i64 %i, 1
  br label %H
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
  EXPECT_EQ(observationCount(Graph, "A"), 0u);
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

TEST(MAIRGraphPass, RedundantObservationDoesNotDeleteSelfReachableAccess) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  const llvm::Instruction &LoadInst = *BB.begin();
  MAIRGraph Graph(F);
  auto &Load = Graph.createInstProgramPoint(BB, LoadInst);
  Graph.addEdge(Load, Load, PPEdgeKind::Dom);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Load.size(), 1u);
}

TEST(MAIRGraphPass, RedundantObservationEliminatesWithinProgramPoint) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p) {
entry:
  %a = load i32, i32* %p, align 4
  %b = load i32, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &FirstLoad = *InstIt++;
  const llvm::Instruction &SecondLoad = *InstIt++;
  MAIRGraph Graph(F);
  auto &PP = Graph.createInstProgramPoint(BB, FirstLoad);
  Observation Second =
      Observation::fromInstruction(SecondLoad, M->getDataLayout());
  PP.addObservation(Second);
  Graph.addEdge(PP, PP, PPEdgeKind::Dom);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(PP.size(), 1u);
}

TEST(MAIRGraphPass, RedundantObservationUsesCoverageOrderForWitness) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %narrow = load i32, ptr %p, align 4
  %wide = load i64, ptr %p, align 8
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &NarrowLoad = *InstIt++;
  const llvm::Instruction &WideLoad = *InstIt++;
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createInstProgramPoint(BB, NarrowLoad);
  PP.addObservation(Observation::fromInstruction(WideLoad, M->getDataLayout()));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(PP.size(), 1u);
  const AbstractAccess *Region = PP.begin()->getContiguousRegion();
  ASSERT_NE(Region, nullptr);
  ASSERT_TRUE(Region->getSize().isConstant());
  EXPECT_EQ(Region->getSize().getConstant(), 8u);
}

TEST(MAIRGraphPass, RedundantObservationOrdersEqualLocationsByCoverageLevel) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %read = load i32, ptr %p, align 4
  store i32 0, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &Read = *InstIt++;
  const llvm::Instruction &Write = *InstIt++;
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createInstProgramPoint(BB, Read);
  PP.addObservation(Observation::fromInstruction(Write, M->getDataLayout()));
  ASSERT_LT(PP.begin()->getLevel(), std::next(PP.begin())->getLevel());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(PP.size(), 1u);
  EXPECT_TRUE(PP.begin()->isWrite());
}

TEST(MAIRGraphPass, RedundantObservationUsesNonAdjacentLocalWitness) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %wide = load i128, ptr %p, align 16
  %p4 = getelementptr i8, ptr %p, i64 4
  %atomic = load atomic i32, ptr %p4 monotonic, align 4
  %p8 = getelementptr i8, ptr %p, i64 8
  %narrow = load i32, ptr %p8, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &WideLoad = *InstIt++;
  ++InstIt;
  const llvm::Instruction &AtomicLoad = *InstIt++;
  ++InstIt;
  const llvm::Instruction &NarrowLoad = *InstIt++;
  MAIRGraph Graph(F);
  auto &PP = Graph.createInstProgramPoint(BB, WideLoad);
  Observation Atomic =
      Observation::fromInstruction(AtomicLoad, M->getDataLayout());
  Observation Narrow =
      Observation::fromInstruction(NarrowLoad, M->getDataLayout());
  PP.addObservation(Atomic);
  PP.addObservation(Narrow);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(PP.size(), 2u);
  EXPECT_FALSE(PP.begin()->isAtomic());
  EXPECT_TRUE(std::next(PP.begin())->isAtomic());
}

TEST(MAIRGraphPass, RedundantObservationLocalCoverageRespectsLevelDirection) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %wide = load i64, ptr %p, align 8
  store i32 0, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &WideLoad = *InstIt++;
  const llvm::Instruction &NarrowStore = *InstIt++;
  MAIRGraph Graph(F);
  auto &PP = Graph.createInstProgramPoint(BB, WideLoad);
  Observation Store =
      Observation::fromInstruction(NarrowStore, M->getDataLayout());
  PP.addObservation(Store);

  EXPECT_LT(PP.begin()->getLevel(), std::next(PP.begin())->getLevel());
  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(PP.size(), 2u);
}

TEST(MAIRGraphPass, RedundantObservationUnionsAllPredecessorFacts) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p, i32* %q) {
entry:
  %p0 = load i32, i32* %p, align 4
  %q0 = load i32, i32* %q, align 4
  %p1 = load i32, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &PLoad = *InstIt++;
  const llvm::Instruction &QLoad = *InstIt++;
  const llvm::Instruction &RedundantPLoad = *InstIt++;
  MAIRGraph Graph(F);
  auto &P = Graph.createInstProgramPoint(BB, PLoad);
  auto &Q = Graph.createInstProgramPoint(BB, QLoad);
  auto &Merge = Graph.createInstProgramPoint(BB, RedundantPLoad);
  Graph.addEdge(P, Merge, PPEdgeKind::Dom);
  Graph.addEdge(Q, Merge, PPEdgeKind::PostDom);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(P.size(), 1u);
  EXPECT_EQ(Q.size(), 1u);
  EXPECT_TRUE(Merge.empty());
}

TEST(MAIRGraphPass, RedundantReadWriteAccessCoversLowerLevels) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p) {
entry:
  %a = load i32, i32* %p, align 4
  %b = load i32, i32* %p, align 4
  store i32 %b, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &FirstLoad = *InstIt++;
  const llvm::Instruction &SecondLoad = *InstIt++;
  const llvm::Instruction &StoreInst = *InstIt++;
  MAIRGraph Graph(F);
  auto &ReadWrite = Graph.createInstProgramPoint(BB, FirstLoad);
  ReadWrite.begin()->mergeKind(AccessKind::Write);
  auto &Read = Graph.createInstProgramPoint(BB, SecondLoad);
  auto &Write = Graph.createInstProgramPoint(BB, StoreInst);
  EXPECT_GT(ReadWrite.begin()->getLevel(), Write.begin()->getLevel());
  EXPECT_GT(Write.begin()->getLevel(), Read.begin()->getLevel());
  Graph.addEdge(ReadWrite, Read, PPEdgeKind::Dom);
  Graph.addEdge(Read, Write, PPEdgeKind::Dom);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(ReadWrite.size(), 1u);
  EXPECT_TRUE(Read.empty());
  EXPECT_TRUE(Write.empty());
}

TEST(MAIRGraphPass, RedundantObservationKeepsMismatchedAccessKinds) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %value = load i32, ptr %p, align 4
  store i32 %value, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &Load = *InstIt++;
  const llvm::Instruction &Store = *InstIt++;
  MAIRGraph Graph(F);
  auto &Read = Graph.createInstProgramPoint(BB, Load);
  auto &Write = Graph.createInstProgramPoint(BB, Store);
  Graph.addEdge(Read, Write, PPEdgeKind::Dom);

  Observation Copy = Observation::fromInstruction(Load, M->getDataLayout());
  expectMergeRejectedWithoutMutation(Copy, *Write.begin());
  EXPECT_EQ(Copy.getLevel(), 1u);
  Copy.mergeKind(AccessKind::Write);
  EXPECT_EQ(Copy.getKind(), AccessKind::ReadWrite);
  EXPECT_EQ(Copy.getLevel(), 3u);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Read.size(), 1u);
  EXPECT_EQ(Write.size(), 1u);
}

TEST(MAIRGraphPass, RedundantObservationKeepsAtomicAccesses) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %plain = load i32, ptr %p, align 4
  %atomic = load atomic i32, ptr %p seq_cst, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &PlainLoad = *InstIt++;
  const llvm::Instruction &AtomicLoad = *InstIt++;
  MAIRGraph Graph(F);
  auto &Plain = Graph.createInstProgramPoint(BB, PlainLoad);
  auto &Atomic = Graph.createInstProgramPoint(BB, AtomicLoad);
  Graph.addEdge(Plain, Atomic, PPEdgeKind::Dom);

  Observation Copy =
      Observation::fromInstruction(PlainLoad, M->getDataLayout());
  expectMergeRejectedWithoutMutation(Copy, *Atomic.begin());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Plain.size(), 1u);
  EXPECT_EQ(Atomic.size(), 1u);
}

struct CallIsBarrier {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::CallBase>(I);
  }
  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return isForwardBarrier(I);
  }
};

TEST(MAIRBarrierAnalysis, CountsBarriersOfAnEndpointThatRunsAgain) {
  llvm::LLVMContext Ctx;
  // @latch_side: B post-dominates H, but H can run again before B does, and
  // then the call in H lies between the earlier store and the store in B.
  // @plain: the same loop without the call; B covers the store in H.
  // @cycle_side: entry dominates A, but A can run again without entry, and
  // then the call in A lies between the store in entry and the store in A.
  auto M = parseModule(R"IR(
declare void @barrier()
define void @latch_side(ptr %p, i64 %n) {
entry:
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %A ]
  call void @barrier()
  store i32 0, ptr %p, align 4
  %c = icmp ult i64 %i, 3
  br i1 %c, label %A, label %B
B:
  store i32 1, ptr %p, align 4
  %c2 = icmp eq i64 %i, %n
  br i1 %c2, label %exit, label %A
A:
  %i.next = add i64 %i, 1
  br label %H
exit:
  ret void
}
define void @plain(ptr %p, i64 %n) {
entry:
  br label %H
H:
  %i = phi i64 [ 0, %entry ], [ %i.next, %A ]
  store i32 0, ptr %p, align 4
  %c = icmp ult i64 %i, 3
  br i1 %c, label %A, label %B
B:
  store i32 1, ptr %p, align 4
  %c2 = icmp eq i64 %i, %n
  br i1 %c2, label %exit, label %A
A:
  %i.next = add i64 %i, 1
  br label %H
exit:
  ret void
}
define void @cycle_side(ptr %p, i1 %c, i1 %c1) {
entry:
  store i32 0, ptr %p, align 4
  br i1 %c, label %A, label %D
A:
  store i32 1, ptr %p, align 4
  call void @barrier()
  br label %D
D:
  br i1 %c1, label %A, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  auto Block = [](llvm::Function &F,
                  llvm::StringRef Name) -> const llvm::BasicBlock & {
    for (const llvm::BasicBlock &BB : F)
      if (BB.getName() == Name)
        return BB;
    llvm_unreachable("missing block");
  };
  auto Run = [&](llvm::StringRef Name) {
    llvm::PassInstrumentationCallbacks PIC;
    llvm::FunctionAnalysisManager FAM;
    registerLoopAnalyses(FAM, PIC);
    MAIRGraph Graph = createGraphWithFunctionAnalyses<CallIsBarrier>(
        *M->getFunction(Name), FAM,
        MAIROptimizationConfig::forThreadSanitizer());
    EXPECT_TRUE(Graph.verify());
    MAIRGraphAnalysisManager AM;
    runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
    EXPECT_TRUE(Graph.verify());
    return std::make_pair(
        observationCount(Graph, Name == "cycle_side" ? "A" : "H"),
        observationCount(Graph, "B"));
  };

  {
    llvm::Function &F = *M->getFunction("latch_side");
    MAIRBarrierAnalysis<CallIsBarrier> Analysis;
    EXPECT_TRUE(Analysis.hasBwdBarrierPath(Block(F, "H"), Block(F, "B")));
    // Dominance is unaffected: no call runs between a store in H and the
    // next store in B.
    EXPECT_FALSE(Analysis.hasFwdBarrierPath(Block(F, "H"), Block(F, "B")));
  }
  {
    llvm::Function &F = *M->getFunction("cycle_side");
    MAIRBarrierAnalysis<CallIsBarrier> Analysis;
    EXPECT_TRUE(Analysis.hasFwdBarrierPath(Block(F, "entry"), Block(F, "A")));
  }
  // The store in H stays; it covers the one in B by dominance.
  EXPECT_EQ(Run("latch_side"), std::make_pair(1u, 0u));
  // Without the call, B covers H first.
  EXPECT_EQ(Run("plain"), std::make_pair(0u, 1u));
  EXPECT_EQ(Run("cycle_side").first, 1u);
}

// Nested irreducible cycles: each level is entered at its header and at its
// latch. The store in the innermost block uses a pointer defined inside every
// cycle; the one in the second innermost latch is outside the innermost.
std::string nestedIrreducibleCycles(unsigned Depth) {
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  OS << "define void @f(i1 %c) {\nentry:\n"
     << "  br i1 %c, label %header0, label %latch0\n";
  for (unsigned I = 0; I < Depth; ++I) {
    OS << "header" << I << ":\n";
    if (I + 1 < Depth)
      OS << "  br i1 %c, label %header" << I + 1 << ", label %latch" << I + 1
         << "\n";
    else
      OS << "  br label %body\n";
  }
  OS << "body:\n  %p = alloca i32\n  store i32 1, ptr %p, align 4\n"
     << "  br label %latch" << Depth - 1 << "\n";
  for (unsigned I = Depth; I-- > 0;) {
    OS << "latch" << I << ":\n";
    if (I + 2 == Depth)
      OS << "  %q = alloca i32\n  store i32 2, ptr %q, align 4\n";
    OS << "  br i1 %c, label %header" << I << ", label %";
    if (I)
      OS << "latch" << I - 1;
    else
      OS << "exit";
    OS << "\n";
  }
  OS << "exit:\n  ret void\n}\n";
  return IR;
}

TEST(RedundantObservationEliminationPass, HandlesManyIteratedCycles) {
  // With 64 cycles the first store is variant in all of them and the second
  // in all but one; 65 exceeds what the analysis tracks.
  for (unsigned Depth : {63u, 64u, 65u}) {
    llvm::LLVMContext Ctx;
    auto M = parseModule(nestedIrreducibleCycles(Depth), Ctx);
    ASSERT_TRUE(M);
    MAIRGraph Graph = buildGraph(*M->getFunction("f"),
                                 MAIROptimizationConfig::forThreadSanitizer());
    ASSERT_TRUE(Graph.verify());
    MAIRGraphAnalysisManager AM;
    runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
    unsigned Kept = 0;
    for (const ProgramPoint &PP : testProgramPoints(Graph))
      Kept += PP.size();
    EXPECT_EQ(Kept, 2u) << "depth " << Depth;
    EXPECT_TRUE(Graph.verify());
  }
}

} // namespace
