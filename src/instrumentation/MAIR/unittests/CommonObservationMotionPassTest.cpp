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

TEST(CommonObservationMotionPass, AddressSanitizerHoistsIdenticalReads) {
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
  llvm::Instruction *LeftLoad = &std::next(F.begin())->front();
  llvm::Instruction *RightLoad = &std::next(F.begin(), 2)->front();
  placeBranchReadsOnEntries(Graph, F);
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(CommonObservationMotionPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "left"), 0u);
  EXPECT_EQ(observationCount(Graph, "right"), 0u);
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  const ProgramPoint &Target = Graph.getBBExitRepresentative(F.getEntryBlock());
  ASSERT_EQ(Target.size(), 1u);
  EXPECT_TRUE(Target.hasAlwaysGuard());
  const Observation &Moved = *Target.begin();
  EXPECT_EQ(Moved.getInstructions(),
            (llvm::ArrayRef<llvm::Instruction *>{LeftLoad, RightLoad}));
  ASSERT_NE(Moved.getContiguousRegion(), nullptr);
  EXPECT_EQ(*Moved.getContiguousRegion(),
            AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                           AbstractValue::constant(4)));
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

void moveProgramPointObservationsTo(MAIRGraph &Graph, llvm::StringRef BlockName,
                                    ProgramPoint &Target) {
  ProgramPoint *Source = nullptr;
  for (ProgramPoint &PP : testProgramPoints(Graph))
    if (PP.isMemoryInstSite() && PP.getBasicBlock() &&
        PP.getBasicBlock()->getName() == BlockName) {
      Source = &PP;
      break;
    }
  ASSERT_NE(Source, nullptr);
  while (!Source->empty()) {
    Observation &Obs = *Source->begin();
    Target.addObservation(Obs);
    Source->erase(Obs);
  }
}

TEST(MAIRGraphPass, CommonObservationMotionDoesNotSearchNeighborDescendants) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i1 %cond) {
entry:
  %p = alloca i32, align 4
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
  const llvm::Value &Alloca = *F.getEntryBlock().begin();
  const MemoryObject *Object = Graph.getMemoryObject(Alloca);
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  ProgramPoint &TargetBefore = Graph.getBBExitRepresentative(F.getEntryBlock());
  ProgramPoint &LeftBoundary =
      Graph.getBBEntryRepresentative(*std::next(F.begin()));
  const ProgramPoint *LeftAccess = findMemoryProgramPoint(Graph, "left");
  ASSERT_NE(LeftAccess, nullptr);
  EXPECT_TRUE(LeftBoundary.hasParent(TargetBefore, PPEdgeKind::Dom));
  EXPECT_FALSE(LeftAccess->hasParent(TargetBefore, PPEdgeKind::Dom));

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Target = findBlockExit(Graph, "entry");
  ASSERT_NE(Target, nullptr);
  EXPECT_TRUE(Target->empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
}

TEST(MAIRGraphPass,
     CommonObservationMotionHoistsBoundaryAccessesAndPreservesProvenance) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i1 %cond) {
entry:
  %p = alloca i32, align 4
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
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  const llvm::Instruction *LeftLoad = &Left.front();
  const llvm::Instruction *RightLoad = &Right.front();
  MAIRGraph Graph = buildGraph(F);
  moveProgramPointObservationsTo(Graph, "left",
                                 Graph.getBBEntryRepresentative(Left));
  moveProgramPointObservationsTo(Graph, "right",
                                 Graph.getBBEntryRepresentative(Right));
  const llvm::Value &Alloca = *F.getEntryBlock().begin();
  const MemoryObject *Object = Graph.getMemoryObject(Alloca);
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Target = findBlockExit(Graph, "entry");
  ASSERT_NE(Target, nullptr);
  ASSERT_EQ(Target->size(), 1u);
  EXPECT_EQ(Target->begin()->getInstructions().size(), 2u);
  EXPECT_TRUE(llvm::is_contained(Target->begin()->getInstructions(), LeftLoad));
  EXPECT_TRUE(
      llvm::is_contained(Target->begin()->getInstructions(), RightLoad));
  EXPECT_EQ(llvm::range_size(Object->users()), 1u);
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
}

TEST(MAIRGraphPass, CommonObservationMotionRequiresCommonTargetParent) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %outer, i1 %inner) {
entry:
  br i1 %outer, label %dispatch, label %right
dispatch:
  br i1 %inner, label %left, label %right
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
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::BasicBlock &Dispatch = *std::next(F.begin());
  llvm::BasicBlock &Left = *std::next(F.begin(), 2);
  llvm::BasicBlock &Right = *std::next(F.begin(), 3);
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint &Target = Graph.getBBExitRepresentative(Dispatch);
  ProgramPoint &EntryExit = Graph.getBBExitRepresentative(Entry);
  ProgramPoint &LeftBoundary = Graph.getBBEntryRepresentative(Left);
  ProgramPoint &RightBoundary = Graph.getBBEntryRepresentative(Right);
  moveProgramPointObservationsTo(Graph, "left", LeftBoundary);
  moveProgramPointObservationsTo(Graph, "right", RightBoundary);
  ASSERT_TRUE(LeftBoundary.hasParent(Target, PPEdgeKind::Dom));
  ASSERT_FALSE(RightBoundary.hasParent(Target, PPEdgeKind::Dom));
  ASSERT_TRUE(RightBoundary.hasParent(EntryExit, PPEdgeKind::Dom));
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  ASSERT_EQ(llvm::range_size(Object->users()), 2u);
  ASSERT_TRUE(Graph.verify());

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_TRUE(Target.empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonObservationMotionRequiresCommonTargetParentForSink) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %path, i1 %escape) {
entry:
  br i1 %path, label %left.path, label %right.path
left.path:
  br label %left
right.path:
  br label %right
left:
  %a = load i32, ptr %p, align 4
  br label %merge
right:
  %b = load i32, ptr %p, align 4
  br i1 %escape, label %exit, label %merge
merge:
  ret void
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  llvm::BasicBlock &Left = *std::next(F.begin(), 3);
  llvm::BasicBlock &Right = *std::next(F.begin(), 4);
  llvm::BasicBlock &Merge = *std::next(F.begin(), 5);
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint &Target = Graph.getBBEntryRepresentative(Merge);
  ProgramPoint &LeftBoundary = Graph.getBBExitRepresentative(Left);
  ProgramPoint &RightBoundary = Graph.getBBExitRepresentative(Right);
  moveProgramPointObservationsTo(Graph, "left", LeftBoundary);
  moveProgramPointObservationsTo(Graph, "right", RightBoundary);
  ASSERT_TRUE(LeftBoundary.hasParent(Target, PPEdgeKind::PostDom));
  ASSERT_FALSE(RightBoundary.hasParent(Target, PPEdgeKind::PostDom));
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  ASSERT_EQ(llvm::range_size(Object->users()), 2u);
  ASSERT_TRUE(Graph.verify());

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_TRUE(Target.empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonObservationMotionHoistsPointerFromDominatingBlock) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i64 %i, i1 %cond) {
entry:
  %pi = getelementptr i8, ptr %p, i64 %i
  br label %dispatch
dispatch:
  br i1 %cond, label %left, label %right
left:
  %a = load i8, ptr %pi, align 1
  ret void
right:
  %b = load i8, ptr %pi, align 1
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  llvm::BasicBlock &Left = *std::next(F.begin(), 2);
  llvm::BasicBlock &Right = *std::next(F.begin(), 3);
  moveProgramPointObservationsTo(Graph, "left",
                                 Graph.getBBEntryRepresentative(Left));
  moveProgramPointObservationsTo(Graph, "right",
                                 Graph.getBBEntryRepresentative(Right));
  CommonObservationMotionPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const ProgramPoint *Target = findBlockExit(Graph, "dispatch");
  ASSERT_NE(Target, nullptr);
  ASSERT_EQ(Target->size(), 1u);
  EXPECT_EQ(Target->begin()->getInstructions().size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonObservationMotionAssertsUnavailableCommonAccess) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  %local = alloca i32, align 4
  %a = load i32, ptr %local, align 4
  ret void
right:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint &LeftBoundary = Graph.getBBEntryRepresentative(Left);
  ProgramPoint &RightBoundary = Graph.getBBEntryRepresentative(Right);
  moveProgramPointObservationsTo(Graph, "left", LeftBoundary);
  ASSERT_EQ(LeftBoundary.size(), 1u);
  RightBoundary.addObservation(*LeftBoundary.begin());
  ASSERT_TRUE(Graph.verify());

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
#ifndef NDEBUG
  EXPECT_DEATH_IF_SUPPORTED((void)Pass.run(Graph, AM),
                            "common observation operands must be available");
#endif
}

TEST(MAIRGraphPass, CommonObservationMotionSinksToEntryAndPreservesProvenance) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left.path, label %right.path
left.path:
  br label %left
right.path:
  br label %right
left:
  %a = load i32, ptr %p, align 4
  br label %merge
right:
  %b = load i32, ptr %p, align 4
  br label %merge
merge:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  const llvm::Instruction *LeftLoad = &*std::next(F.begin(), 3)->begin();
  const llvm::Instruction *RightLoad = &*std::next(F.begin(), 4)->begin();
  MAIRGraph Graph = buildGraph(F);
  llvm::BasicBlock &Left = *std::next(F.begin(), 3);
  llvm::BasicBlock &Right = *std::next(F.begin(), 4);
  moveProgramPointObservationsTo(Graph, "left",
                                 Graph.getBBExitRepresentative(Left));
  moveProgramPointObservationsTo(Graph, "right",
                                 Graph.getBBExitRepresentative(Right));
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Target = findBlockEntry(Graph, "merge");
  ASSERT_NE(Target, nullptr);
  ASSERT_EQ(Target->size(), 1u);
  EXPECT_EQ(Target->begin()->getInstructions().size(), 2u);
  EXPECT_TRUE(llvm::is_contained(Target->begin()->getInstructions(), LeftLoad));
  EXPECT_TRUE(
      llvm::is_contained(Target->begin()->getInstructions(), RightLoad));
  EXPECT_EQ(llvm::range_size(Object->users()), 1u);
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
}

TEST(MAIRGraphPass,
     CommonObservationMotionDoesNotSearchPredecessorDescendants) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  %a = load i32, ptr %p, align 4
  br label %merge
right:
  %b = load i32, ptr %p, align 4
  br label %merge
merge:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  MAIRGraph Graph = buildGraph(F);
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  ASSERT_EQ(llvm::range_size(Object->users()), 2u);

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_TRUE(Graph.getBBEntryRepresentative(*std::next(F.begin(), 3)).empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonObservationMotionHoistsAfterForwardBarrierInTarget) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(ptr %p, i1 %cond) {
entry:
  call void @barrier()
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

  llvm::Function &F = *std::next(M->begin());
  MAIRGraph Graph = buildGraph<CallBarrierTrait>(F);
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  moveProgramPointObservationsTo(Graph, "left",
                                 Graph.getBBEntryRepresentative(Left));
  moveProgramPointObservationsTo(Graph, "right",
                                 Graph.getBBEntryRepresentative(Right));
  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;

  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Target = findBlockExit(Graph, "entry");
  ASSERT_NE(Target, nullptr);
  EXPECT_EQ(Target->size(), 1u);
}

TEST(MAIRGraphPass,
     CommonObservationMotionHoistsBeforeForwardBarrierInNeighbor) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  call void @barrier()
  %a = load i32, ptr %p, align 4
  ret void
right:
  %b = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *std::next(M->begin());
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  MAIRGraph Graph = buildGraph<CallBarrierTrait>(F);
  moveProgramPointObservationsTo(Graph, "left",
                                 Graph.getBBEntryRepresentative(Left));
  moveProgramPointObservationsTo(Graph, "right",
                                 Graph.getBBEntryRepresentative(Right));

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.getBBExitRepresentative(F.getEntryBlock()).size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass,
     CommonObservationMotionSinksAfterBackwardBarrierInPredecessor) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(ptr %p, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right
left:
  %a = load i32, ptr %p, align 4
  call void @barrier()
  br label %merge
right:
  %b = load i32, ptr %p, align 4
  br label %merge
merge:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *std::next(M->begin());
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  llvm::BasicBlock &Merge = *std::next(F.begin(), 3);
  MAIRGraph Graph = buildGraph<CallBarrierTrait>(F);
  moveProgramPointObservationsTo(Graph, "left",
                                 Graph.getBBExitRepresentative(Left));
  moveProgramPointObservationsTo(Graph, "right",
                                 Graph.getBBExitRepresentative(Right));

  MAIRGraphAnalysisManager AM;
  CommonObservationMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.getBBEntryRepresentative(Merge).size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
