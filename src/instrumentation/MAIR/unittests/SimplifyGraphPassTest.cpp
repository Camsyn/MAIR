#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIRPassTestUtil.h"

using namespace mair;

namespace {

TEST(SimplifyGraphPass, SimplifyGraphContractsStraightLineReads) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  const size_t Before = Graph.size();
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(SimplifyGraphPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), Before - 1);
  EXPECT_EQ(observationCount(Graph, "entry"), 2u);
  const ProgramPoint *Kept = nullptr;
  for (const ProgramPoint &PP : Graph)
    if (!PP.empty())
      Kept = &PP;
  ASSERT_NE(Kept, nullptr);
  EXPECT_EQ(Kept->size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(SimplifyGraphPass, ContractionMovesObservationsInSourceOrder) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, ptr %q, ptr %r) {
entry:
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %q, align 4
  %third = load i32, ptr %r, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  llvm::SmallVector<const Observation *, 3> Before;
  for (const ProgramPoint &PP : Graph)
    for (const Observation &Obs : PP)
      Before.push_back(&Obs);
  ASSERT_EQ(Before.size(), 3u);

  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(SimplifyGraphPass().run(Graph, AM).areAllPreserved());

  // Contraction relinks the existing observations instead of copying them.
  llvm::SmallVector<const Observation *, 3> After;
  for (const ProgramPoint &PP : Graph)
    for (const Observation &Obs : PP) {
      EXPECT_EQ(Obs.getParent(), &PP);
      EXPECT_EQ(Graph.getMemoryObject(*Obs.getBase()), &Obs.getMemoryObject());
      After.push_back(&Obs);
    }
  EXPECT_EQ(After, Before);
  EXPECT_TRUE(Graph.verify());
}

TEST(SimplifyGraphPass, LeavesASingleReadUnchanged) {
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
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  const size_t Before = Graph.size();
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(SimplifyGraphPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), Before);
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(SimplifyGraphPass, DoesNotContractAcrossALoopBoundary) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %done) {
entry:
  %outside = load i32, ptr %p, align 4
  br label %loop
loop:
  %inside = load i32, ptr %p, align 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  ProgramPoint *Outside = findMutableMemoryProgramPoint(Graph, "entry");
  ProgramPoint *Inside = findMutableMemoryProgramPoint(Graph, "loop");
  ASSERT_NE(Outside, nullptr);
  ASSERT_NE(Inside, nullptr);
  EXPECT_NE(Outside->getParent(), Inside->getParent());
  EXPECT_FALSE(Graph.hasEdge(*Outside, *Inside, PPEdgeKind::Dom));
  EXPECT_FALSE(Graph.hasEdge(*Inside, *Outside, PPEdgeKind::PostDom));
  EXPECT_DEATH_IF_SUPPORTED(Graph.addEdge(*Outside, *Inside, PPEdgeKind::Dom),
                            "graph");
  const size_t Before = Graph.size();
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(SimplifyGraphPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), Before);
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_EQ(observationCount(Graph, "loop"), 1u);
  EXPECT_TRUE(Graph.verify());
}

void expectSimplifyGraphPreservesMutualPair(MAIRGraph &Graph,
                                            ProgramPoint &Earlier,
                                            ProgramPoint &Later) {
  if (Earlier.getParent() != Later.getParent()) {
    EXPECT_FALSE(Graph.hasEdge(Earlier, Later, PPEdgeKind::Dom));
    EXPECT_FALSE(Graph.hasEdge(Later, Earlier, PPEdgeKind::PostDom));
    EXPECT_DEATH_IF_SUPPORTED(Graph.addEdge(Earlier, Later, PPEdgeKind::Dom),
                              "graph");
  } else {
    Earlier.getParent()->addEdge(Earlier, Later, PPEdgeKind::Dom);
    Earlier.getParent()->addEdge(Later, Earlier, PPEdgeKind::PostDom);
  }
  const size_t InitialSize = Graph.size();
  const std::string EarlierName = Earlier.getName().str();
  const std::string LaterName = Later.getName().str();
  ASSERT_TRUE(Graph.verify());

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(runPassOnGraphTree(Pass, Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), InitialSize);
  EXPECT_NE(Earlier.getParent()->find(EarlierName), nullptr);
  EXPECT_NE(Later.getParent()->find(LaterName), nullptr);
  EXPECT_TRUE(Graph.verify());
}

void expectSimplifyGraphContractsMutualPair(MAIRGraph &Graph,
                                            ProgramPoint &Earlier,
                                            ProgramPoint &Later) {
  MAIRGraph &Scope = *Earlier.getParent();
  ASSERT_EQ(&Scope, Later.getParent());
  Scope.addEdge(Earlier, Later, PPEdgeKind::Dom);
  Scope.addEdge(Later, Earlier, PPEdgeKind::PostDom);
  const size_t InitialSize = Scope.size();
  const std::string EarlierName = Earlier.getName().str();
  const std::string LaterName = Later.getName().str();
  ASSERT_TRUE(Graph.verify());

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Scope, AM).areAllPreserved());
  EXPECT_EQ(Scope.size(), InitialSize - 1);
  EXPECT_EQ(Scope.find(EarlierName), nullptr);
  EXPECT_NE(Scope.find(LaterName), nullptr);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, SimplifyGraphMergesMutualDomPairAndRewiresEdges) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p, i32* %q) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %q, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &LoadInst = *InstIt++;
  const llvm::Instruction &StoreInst = *InstIt++;

  MAIRGraph Graph(F);
  auto &Entry = Graph.createBBProgramPoint(BB, true);
  auto &Load = Graph.createInstProgramPoint(BB, LoadInst);
  auto &Store = Graph.createInstProgramPoint(BB, StoreInst);
  auto &Exit = Graph.createBBProgramPoint(BB, false);
  Graph.addEdge(Entry, Load, PPEdgeKind::Dom);
  Graph.addEdge(Load, Store, PPEdgeKind::Dom);
  Graph.addEdge(Store, Load, PPEdgeKind::PostDom);
  Graph.addEdge(Store, Exit, PPEdgeKind::Dom);

  MAIRGraphAnalysisManager AM;
  SimplifyGraphPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 3u);
  EXPECT_EQ(Graph.find("pp1"), nullptr);
  const ProgramPoint *Merged = Graph.find("pp2");
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->observations().size(), 2u);
  EXPECT_TRUE(hasSuccessor(Graph, Entry, *Merged, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merged, Exit, PPEdgeKind::Dom));
}

TEST(MAIRGraphPass, SimplifyGraphSinksPastSymbolicPointerDefinition) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i64 %i) {
entry:
  %first = load i8, ptr %p, align 1
  %pi = getelementptr i8, ptr %p, i64 %i
  %second = load i8, ptr %pi, align 1
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::Instruction *First = &F.getEntryBlock().front();
  llvm::Instruction *Second = &*std::next(F.getEntryBlock().begin(), 2);
  MAIRGraph Graph = buildGraph(F);

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  EXPECT_EQ(Graph.find("pp1"), nullptr);
  const ProgramPoint *Merged = Graph.find("pp2");
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(&Merged->getAnchor(), Second);
  ASSERT_EQ(Merged->size(), 2u);
  auto Obs = Merged->begin();
  EXPECT_EQ(Obs->getInstruction(), First);
  EXPECT_EQ((++Obs)->getInstruction(), Second);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, SimplifyGraphRequiresReversePostDomEdge) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  auto InstIt = BB.begin();
  const llvm::Instruction &LoadInst = *InstIt++;
  const llvm::Instruction &StoreInst = *InstIt++;

  MAIRGraph Graph(F);
  auto &Load = Graph.createInstProgramPoint(BB, LoadInst);
  auto &Store = Graph.createInstProgramPoint(BB, StoreInst);
  Graph.addEdge(Load, Store, PPEdgeKind::Dom);
  Graph.addEdge(Store, Load, PPEdgeKind::Dom);

  MAIRGraphAnalysisManager AM;
  SimplifyGraphPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 2u);
  EXPECT_NE(Graph.find("pp0"), nullptr);
  EXPECT_NE(Graph.find("pp1"), nullptr);
}

TEST(MAIRGraphPass, SimplifyGraphRewiresExternalPostDomEdges) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p, i32* %q) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %q, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  const llvm::Instruction &LoadInst = *BB.begin();
  const llvm::Instruction &StoreInst = *std::next(BB.begin());

  MAIRGraph Graph(F);
  auto &A = Graph.createInstProgramPoint(BB, LoadInst);
  auto &B = Graph.createInstProgramPoint(BB, StoreInst);
  auto &C = Graph.createBBProgramPoint(BB, false);
  auto &D = Graph.createBBProgramPoint(BB, false);
  auto &E = Graph.createBBProgramPoint(BB, false);
  Graph.addEdge(A, B, PPEdgeKind::Dom);
  Graph.addEdge(B, A, PPEdgeKind::PostDom);
  Graph.addEdge(C, B, PPEdgeKind::PostDom);
  Graph.addEdge(B, D, PPEdgeKind::Dom);
  Graph.addEdge(B, E, PPEdgeKind::PostDom);

  MAIRGraphAnalysisManager AM;
  SimplifyGraphPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.find("pp0"), nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, C, B, PPEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, B, D, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, B, E, PPEdgeKind::PostDom));
  EXPECT_FALSE(hasSuccessor(Graph, B, C));
  EXPECT_FALSE(hasSuccessor(Graph, B, E, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, D, B));
}

TEST(MAIRGraphPass, SimplifyGraphPreservesParallelTypedEdges) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  store i32 %v, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &LoadInst = *BB.begin();
  llvm::Instruction &StoreInst = *std::next(BB.begin());
  MAIRGraph Graph(F);
  ProgramPoint &Earlier = Graph.createInstProgramPoint(BB, LoadInst);
  ProgramPoint &Later = Graph.createInstProgramPoint(BB, StoreInst);
  ProgramPoint &External = Graph.createBBProgramPoint(BB, false);
  Graph.addEdge(Earlier, Later, PPEdgeKind::Dom);
  Graph.addEdge(Later, Earlier, PPEdgeKind::PostDom);
  Graph.addEdge(Later, External, PPEdgeKind::Dom);
  Graph.addEdge(Later, External, PPEdgeKind::PostDom);
  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.size(), 2u);
  EXPECT_TRUE(Graph.hasEdge(Later, External, PPEdgeKind::Dom));
  EXPECT_TRUE(Graph.hasEdge(Later, External, PPEdgeKind::PostDom));
}

TEST(MAIRGraphPass, SimplifyGraphKeepsCrossBlockBoundaryRepresentatives) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  %a = load i32, ptr %p, align 4
  ret void
right:
  %b = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  MAIRGraph Graph = buildGraph(F);
  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());

  ASSERT_TRUE(Graph.verify());
  for (const llvm::BasicBlock &BB : F) {
    const ProgramPoint &Entry = Graph.getBBEntryRepresentative(BB);
    const ProgramPoint &Exit = Graph.getBBExitRepresentative(BB);
    EXPECT_TRUE(Entry.isBasicBlockEntrySite());
    EXPECT_TRUE(Exit.isBasicBlockExitSite());
    EXPECT_TRUE(Entry.representsBlock(BB));
    EXPECT_TRUE(Exit.representsBlock(BB));
  }
}

TEST(MAIRGraphPass, SimplifyGraphDoesNotCrossLoopBoundary) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %done) {
entry:
  %outside = load i32, ptr %p, align 4
  br label %loop
loop:
  %inside = load i32, ptr %p, align 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint *Outside = findMutableMemoryProgramPoint(Graph, "entry");
  ProgramPoint *Inside = findMutableMemoryProgramPoint(Graph, "loop");
  ASSERT_NE(Outside, nullptr);
  ASSERT_NE(Inside, nullptr);
  expectSimplifyGraphPreservesMutualPair(Graph, *Outside, *Inside);
}

TEST(MAIRGraphPass, SimplifyGraphContractsOutsideAnInterveningLoop) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %take.loop, i1 %done) {
entry:
  %before = load i32, ptr %p, align 4
  br i1 %take.loop, label %loop, label %direct
direct:
  br label %exit
loop:
  br i1 %done, label %exit, label %loop
exit:
  %after = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint *Before = findMutableMemoryProgramPoint(Graph, "entry");
  ProgramPoint *After = findMutableMemoryProgramPoint(Graph, "exit");
  ASSERT_NE(Before, nullptr);
  ASSERT_NE(After, nullptr);
  expectSimplifyGraphContractsMutualPair(Graph, *Before, *After);
}

TEST(MAIRGraphPass, SimplifyGraphDoesNotCrossNestedLoopBoundary) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %outer.header
outer.header:
  %oi = phi i32 [ 0, %entry ], [ %oi.next, %outer.latch ]
  %outer = load i32, ptr %p, align 4
  br label %inner.header
inner.header:
  %ii = phi i32 [ 0, %outer.header ], [ %ii.next, %inner.header ]
  %inner = load i32, ptr %p, align 4
  %ii.next = add i32 %ii, 1
  %inner.done = icmp eq i32 %ii.next, 2
  br i1 %inner.done, label %outer.latch, label %inner.header
outer.latch:
  %oi.next = add i32 %oi, 1
  %outer.done = icmp eq i32 %oi.next, 2
  br i1 %outer.done, label %exit, label %outer.header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint *Outer = findMutableMemoryProgramPoint(Graph, "outer.header");
  ProgramPoint *Inner = findMutableMemoryProgramPoint(Graph, "inner.header");
  ASSERT_NE(Outer, nullptr);
  ASSERT_NE(Inner, nullptr);
  expectSimplifyGraphPreservesMutualPair(Graph, *Outer, *Inner);
}

TEST(MAIRGraphPass, SimplifyGraphContractsAcrossNestedLoopAtSameLevel) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %outer.header
outer.header:
  %oi = phi i32 [ 0, %entry ], [ %oi.next, %outer.latch ]
  %before = load i32, ptr %p, align 4
  br label %inner.header
inner.header:
  %ii = phi i32 [ 0, %outer.header ], [ %ii.next, %inner.header ]
  %ii.next = add i32 %ii, 1
  %inner.done = icmp eq i32 %ii.next, 2
  br i1 %inner.done, label %outer.latch, label %inner.header
outer.latch:
  %after = load i32, ptr %p, align 4
  %oi.next = add i32 %oi, 1
  %outer.done = icmp eq i32 %oi.next, 2
  br i1 %outer.done, label %exit, label %outer.header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint *Before = findMutableMemoryProgramPoint(Graph, "outer.header");
  ProgramPoint *After = findMutableMemoryProgramPoint(Graph, "outer.latch");
  ASSERT_NE(Before, nullptr);
  ASSERT_NE(After, nullptr);
  expectSimplifyGraphContractsMutualPair(Graph, *Before, *After);
}

TEST(MAIRGraphPass, SimplifyGraphCanContractWithinSameLoop) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %done) {
entry:
  br label %loop.header
loop.header:
  %first = load i32, ptr %p, align 4
  br label %loop.latch
loop.latch:
  %second = load i32, ptr %p, align 4
  br i1 %done, label %exit, label %loop.header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint *First = findMutableMemoryProgramPoint(Graph, "loop.header");
  ProgramPoint *Second = findMutableMemoryProgramPoint(Graph, "loop.latch");
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  MAIRGraph &Body = *First->getParent();
  ASSERT_EQ(Second->getParent(), &Body);
  Body.addEdge(*First, *Second, PPEdgeKind::Dom);
  Body.addEdge(*Second, *First, PPEdgeKind::PostDom);
  const size_t InitialSize = Body.size();
  const std::string FirstName = First->getName().str();
  ASSERT_TRUE(Graph.verify());

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Body, AM).areAllPreserved());

  EXPECT_EQ(Body.size(), InitialSize - 1);
  EXPECT_EQ(Body.find(FirstName), nullptr);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, SimplifyGraphAllowsPinnedAccessAtMotionTarget) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %movable = load i32, ptr %p, align 4
  %pinned = load atomic i32, ptr %p seq_cst, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &MovableInst = BB.front();
  llvm::Instruction &PinnedInst = *std::next(BB.begin());
  MAIRGraph Graph(F);
  ProgramPoint &Movable = Graph.createInstProgramPoint(BB, MovableInst);
  ProgramPoint &Pinned = Graph.createInstProgramPoint(BB, PinnedInst);
  ASSERT_TRUE(Movable.begin()->isMovable());
  ASSERT_FALSE(Pinned.begin()->isMovable());

  expectSimplifyGraphContractsMutualPair(Graph, Movable, Pinned);
  EXPECT_EQ(Pinned.size(), 2u);
}

TEST(MAIRGraphPass, SimplifyGraphRejectsPinnedAccessAtMotionSource) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %pinned = load atomic i32, ptr %p seq_cst, align 4
  %movable = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &PinnedInst = BB.front();
  llvm::Instruction &MovableInst = *std::next(BB.begin());
  MAIRGraph Graph(F);
  ProgramPoint &Pinned = Graph.createInstProgramPoint(BB, PinnedInst);
  ProgramPoint &Movable = Graph.createInstProgramPoint(BB, MovableInst);
  ASSERT_FALSE(Pinned.begin()->isMovable());
  ASSERT_TRUE(Movable.begin()->isMovable());

  expectSimplifyGraphPreservesMutualPair(Graph, Pinned, Movable);
}

TEST(MAIRGraphPass, SimplifyGraphPreservesGuardAndEdgeSiteBoundaries) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::BasicBlock &Exit = *std::next(F.begin());
  llvm::Instruction &Load = Entry.front();
  MAIRGraph Graph(F);
  ProgramPoint &Anchored = Graph.createBBProgramPoint(Entry, true);
  ProgramPoint &DifferentGuard = Graph.createProgramPoint(
      Anchored.getSite(), AnyProvenanceExecutedGuard({&Load}));
  ProgramPoint &Edge = Graph.createEdgeProgramPoint(Entry, Exit);
  Graph.addEdge(Anchored, DifferentGuard, PPEdgeKind::Dom);
  Graph.addEdge(DifferentGuard, Anchored, PPEdgeKind::PostDom);
  Graph.addEdge(Anchored, Edge, PPEdgeKind::Dom);
  Graph.addEdge(Edge, Anchored, PPEdgeKind::PostDom);

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), 3u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, SimplifyGraphPreservesDifferentEdgeSites) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i1 %c) {
entry:
  br i1 %c, label %left, label %right
left:
  ret void
right:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  MAIRGraph Graph(F);
  ProgramPoint &LeftEdge = Graph.createEdgeProgramPoint(Entry, Left);
  ProgramPoint &RightEdge = Graph.createEdgeProgramPoint(Entry, Right);
  Graph.addEdge(LeftEdge, RightEdge, PPEdgeKind::Dom);
  Graph.addEdge(RightEdge, LeftEdge, PPEdgeKind::PostDom);

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, SimplifyGraphPreservesBoundaryRepresentativeKinds) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &Load = BB.front();
  MAIRGraph Graph(F);
  ProgramPoint &Entry = Graph.createBBProgramPoint(BB, true);
  ProgramPoint &Memory = Graph.createInstProgramPoint(BB, Load);
  ProgramPoint &Exit = Graph.createBBProgramPoint(BB, false);
  Graph.addEdge(Entry, Memory, PPEdgeKind::Dom);
  Graph.addEdge(Memory, Entry, PPEdgeKind::PostDom);
  Graph.addEdge(Memory, Exit, PPEdgeKind::Dom);
  Graph.addEdge(Exit, Memory, PPEdgeKind::PostDom);

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(&Graph.getBBEntryRepresentative(BB), &Entry);
  EXPECT_EQ(&Graph.getBBExitRepresentative(BB), &Exit);
  EXPECT_TRUE(Graph.getBBEntryRepresentative(BB).isBasicBlockEntrySite());
  EXPECT_TRUE(Graph.getBBExitRepresentative(BB).isBasicBlockExitSite());
  EXPECT_EQ(Graph.size(), 3u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
