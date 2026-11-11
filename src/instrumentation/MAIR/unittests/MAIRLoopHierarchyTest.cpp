#include "MAIR/MAOpt.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIRPassTestUtil.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Instructions.h"

using namespace mair;

namespace {

constexpr llvm::StringLiteral NestedIR = R"IR(
define void @f(ptr %p) {
entry:
  %before = load i32, ptr %p, align 4
  br label %outer
outer:
  %oi = phi i32 [ 0, %entry ], [ %onext, %outer.latch ]
  %outside.inner = load i32, ptr %p, align 4
  br label %inner
inner:
  %ii = phi i32 [ 0, %outer ], [ %inext, %inner ]
  %inside = load i32, ptr %p, align 4
  %inext = add i32 %ii, 1
  %idone = icmp eq i32 %inext, 2
  br i1 %idone, label %outer.latch, label %inner
outer.latch:
  %after.inner = load i32, ptr %p, align 4
  %onext = add i32 %oi, 1
  %odone = icmp eq i32 %onext, 2
  br i1 %odone, label %exit, label %outer
exit:
  %after = load i32, ptr %p, align 4
  ret void
}
)IR";

TEST(MAIRLoopHierarchy, NestedBodiesHaveTypedBoundariesAndLocalTraversal) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(NestedIR, Ctx);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("f");
  MAIRGraph Graph = buildGraph(Function);
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.children().size(), 1u);
  MAIRGraph &Outer = *Graph.children().front();
  ASSERT_EQ(Outer.children().size(), 1u);
  MAIRGraph &Inner = *Outer.children().front();

  EXPECT_EQ(Graph.getLoop(), nullptr);
  EXPECT_EQ(Graph.getParent(), nullptr);
  EXPECT_EQ(Outer.getParent(), &Graph);
  EXPECT_EQ(Inner.getParent(), &Outer);
  EXPECT_EQ(&Inner.getRoot(), &Graph);
  EXPECT_EQ(Graph.getLoopGraph(*Inner.getLoop()), &Inner);
  EXPECT_EQ(Outer.getLoop()->getHeader()->getName(), "outer");
  EXPECT_EQ(Inner.getLoop()->getHeader()->getName(), "inner");

  for (MAIRGraph *Body : {&Outer, &Inner}) {
    LoopProgramPoint *Preheader = Body->getLoopPreheader();
    ASSERT_NE(Preheader, nullptr);
    EXPECT_TRUE(llvm::isa<LoopProgramPoint>(Preheader));
    EXPECT_TRUE(Preheader->isLoopPreheader());
    EXPECT_TRUE(Preheader->isBasicBlockExitSite());
    EXPECT_EQ(&Preheader->getLoopGraph(), Body);
    EXPECT_EQ(Preheader->getParent(), Body->getParent());
    EXPECT_NE(Preheader, &Body->getParent()->getBBExitRepresentative(
                             *Body->getLoop()->getLoopPreheader()));
    ASSERT_EQ(Body->getLoopExits().size(), 1u);
    LoopProgramPoint *Exit = Body->getLoopExits().front();
    EXPECT_TRUE(Exit->isLoopExit());
    EXPECT_TRUE(Exit->isBasicBlockEntrySite());
    EXPECT_EQ(&Exit->getLoopGraph(), Body);
    EXPECT_EQ(Exit->getParent(), Body->getParent());
  }

  llvm::SmallPtrSet<const ProgramPoint *, 32> Points;
  llvm::StringSet<> Names;
  size_t LocalSize = 0;
  for (const MAIRGraph *Current : Graph.graphs()) {
    LocalSize += Current->size();
    for (const ProgramPoint &Point : *Current) {
      EXPECT_EQ(Point.getParent(), Current);
      EXPECT_TRUE(Points.insert(&Point).second);
      EXPECT_TRUE(Names.insert(Point.getName()).second);
      for (const ProgramPointEdge &Edge : Point.children())
        EXPECT_EQ(Edge.getNode().getParent(), Current);
      if (Point.isMemoryInstSite())
        EXPECT_EQ(Graph.getLoopInfo().getLoopFor(Point.getBasicBlock()),
                  Current->getLoop());
    }
  }
  EXPECT_EQ(Graph.graphs().size(), 3u);
  EXPECT_EQ(LocalSize, Graph.allProgramPoints().size());
  EXPECT_EQ(Points.size(), LocalSize);
  for (const ProgramPoint *Point : Graph.allProgramPoints())
    EXPECT_EQ(Points.erase(Point), 1u);
  EXPECT_TRUE(Points.empty());
}

TEST(MAIRLoopHierarchy, MultipleAndMissingLoopExitsAreRepresented) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(R"IR(
define void @multiple(i1 %leave, i1 %which) {
entry:
  br label %loop
loop:
  br i1 %leave, label %exit.a, label %latch
latch:
  br i1 %which, label %exit.b, label %loop
exit.a:
  ret void
exit.b:
  ret void
}
define void @infinite(ptr %p) {
entry:
  br label %loop
loop:
  %value = load i32, ptr %p, align 4
  br label %loop
}
)IR",
                            Ctx);
  ASSERT_TRUE(Module);
  for (llvm::StringRef Name : {"multiple", "infinite"}) {
    MAIRGraph Graph = buildGraph(*Module->getFunction(Name));
    ASSERT_TRUE(Graph.verify());
    ASSERT_EQ(Graph.children().size(), 1u);
    MAIRGraph &Body = *Graph.children().front();
    ASSERT_NE(Body.getLoopPreheader(), nullptr);
    EXPECT_EQ(Body.getLoopExits().size(), Name == "multiple" ? 2u : 0u);
    for (LoopProgramPoint *Exit : Body.getLoopExits())
      EXPECT_EQ(&Exit->getLoopGraph(), &Body);
  }
}

TEST(MAIRLoopHierarchy, OrdinaryRedundancyStaysWithinTheLoopBody) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(R"IR(
define void @f(ptr %p, i1 %done) {
entry:
  %outside = load i32, ptr %p, align 4
  br label %loop
loop:
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %p, align 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
)IR",
                            Ctx);
  ASSERT_TRUE(Module);
  MAIRGraph Graph = buildGraph(*Module->getFunction("f"),
                               MAIROptimizationConfig::forAddressSanitizer());
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, nullptr);
  EXPECT_FALSE(
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_EQ(observationCount(Graph, "entry"), 1u);
  EXPECT_EQ(observationCount(Graph, "loop"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRLoopHierarchy, RootMovesPreserveBodiesMemoryUsesAndExpressions) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(NestedIR, Ctx);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("f");
  MAIRGraph Graph = buildGraph(Function);
  MAIRGraph *Outer = Graph.children().front().get();
  MAIRGraph *Inner = Outer->children().front().get();
  const MemoryObject *Object = Graph.getMemoryObject(*Function.getArg(0));
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 5u);
  EXPECT_EQ(Inner->getMemoryObject(*Function.getArg(0)), Object);
  MAIRScalarExpr Expression = Inner->createValueExpr(*Function.getArg(0));
  EXPECT_TRUE(Graph.ownsScalarExpr(Expression));
  EXPECT_TRUE(Outer->ownsScalarExpr(Expression));
  llvm::Type &IntegerType = *llvm::Type::getInt64Ty(Ctx);
  MAIRScalarExpr Stride =
      Inner->createConstantExpr(IntegerType, llvm::APInt(64, 4));
  MAIRScalarExpr Count =
      Inner->createConstantExpr(IntegerType, llvm::APInt(64, 2));
  ProgramPoint *Source = findMutableMemoryProgramPoint(Graph, "inner");
  ASSERT_NE(Source, nullptr);
  Observation &Original = *Source->begin();
  Inner->getLoopExits().front()->addObservation(
      Observation(Original.getInstructions(),
                  MemoryRegion(PeriodicRegion(Function.getArg(0), Expression,
                                              Stride, Stride, Count)),
                  AccessKind::Read));
  Source->erase(Original);
  ASSERT_TRUE(Graph.verify());

  MAIRGraph Moved(std::move(Graph));
  EXPECT_EQ(Moved.children().front().get(), Outer);
  EXPECT_EQ(Outer->getParent(), &Moved);
  EXPECT_EQ(Inner->getParent(), Outer);
  EXPECT_EQ(&Inner->getRoot(), &Moved);
  EXPECT_EQ(Moved.getMemoryObject(*Function.getArg(0)), Object);
  EXPECT_TRUE(Moved.ownsScalarExpr(Expression));
  EXPECT_TRUE(Moved.verify());

  MAIRGraph Assigned(Function);
  Assigned = std::move(Moved);
  EXPECT_EQ(Outer->getParent(), &Assigned);
  EXPECT_EQ(&Inner->getRoot(), &Assigned);
  EXPECT_EQ(llvm::range_size(Object->users()), 5u);
  EXPECT_TRUE(Assigned.ownsScalarExpr(Expression));
  EXPECT_TRUE(Assigned.verify());
}

TEST(MAIRLoopHierarchy, NormalizationReportsPreheaderAndDedicatedExitChanges) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(R"IR(
define void @f(ptr %p, i1 %take, i1 %done) {
entry:
  br i1 %take, label %loop, label %exit
loop:
  %value = load i32, ptr %p, align 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
)IR",
                            Ctx);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAOpt<> First(Function, FAM, &PIC, true);
  MAIRGraph &Graph = First.build();
  ASSERT_TRUE(First.hasModifiedIR());
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.children().size(), 1u);
  const llvm::Loop *Loop = Graph.children().front()->getLoop();
  ASSERT_NE(Loop->getLoopPreheader(), nullptr);
  EXPECT_NE(Loop->getLoopPreheader(), &Function.getEntryBlock());
  EXPECT_TRUE(Loop->hasDedicatedExits());
  ASSERT_NE(Loop->getUniqueExitBlock(), nullptr);
  EXPECT_NE(Loop->getUniqueExitBlock()->getName(), "exit");
  MAOpt<> Second(Function, FAM, &PIC, true);
  Second.build();
  EXPECT_FALSE(Second.hasModifiedIR());
  EXPECT_TRUE(Second.getGraph().verify());
}

TEST(MAIRLoopHierarchy, RetainingAnInnerObservationUpdatesTheWholeTree) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(NestedIR, Ctx);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAOpt<> Opt(Function, FAM, &PIC, true);
  MAIRGraph &Graph = Opt.build();
  ProgramPoint *InnerPoint = findMutableMemoryProgramPoint(Graph, "inner");
  ASSERT_NE(InnerPoint, nullptr);
  ASSERT_EQ(InnerPoint->size(), 1u);
  const Observation *Selected = &*InnerPoint->begin();
  llvm::SmallPtrSet<const Observation *, 1> Keep;
  Keep.insert(Selected);
  Opt.retainObservations(Keep);
  unsigned Count = 0;
  for (const ProgramPoint *Point : Graph.allProgramPoints())
    for (const Observation &Obs : *Point) {
      EXPECT_EQ(&Obs, Selected);
      EXPECT_EQ(Obs.getParent(), Point);
      ++Count;
    }
  EXPECT_EQ(Count, 1u);
  const MemoryObject *Object = Graph.getMemoryObject(*Function.getArg(0));
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 1u);
  EXPECT_TRUE(Graph.verify());
  const llvm::Instruction *Source = Selected->getInstructions().front();
  ASSERT_FALSE(Opt.optimize());
  unsigned OptimizedCount = 0;
  for (const ProgramPoint *Point : Graph.allProgramPoints())
    for (const Observation &Obs : *Point) {
      EXPECT_EQ(Obs.getParent(), Point);
      ASSERT_EQ(Obs.getInstructions().size(), 1u);
      EXPECT_EQ(Obs.getInstructions().front(), Source);
      ++OptimizedCount;
    }
  EXPECT_EQ(OptimizedCount, 1u);
  EXPECT_EQ(llvm::range_size(Object->users()), 1u);
  EXPECT_TRUE(Graph.verify());
  Keep.clear();
  Opt.retainObservations(Keep);
  for (const ProgramPoint *Point : Graph.allProgramPoints())
    EXPECT_TRUE(Point->empty());
  EXPECT_TRUE(Object->use_empty());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRLoopHierarchy, SourceSelectionPreservesConflictsAndCollectionTiming) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
define void @f(ptr %p, ptr %q, i1 %again) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 4, i1 false)
  %read = load i32, ptr %p, align 4
  br label %loop
loop:
  %redundant = load i32, ptr %p, align 4
  br i1 %again, label %loop, label %exit
exit:
  store i32 %read, ptr %q, align 4
  %readwrite = atomicrmw add ptr %q, i32 1 monotonic
  ret void
}
)IR",
                            Ctx);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("f");
  llvm::BasicBlock &Block = Function.getEntryBlock();
  llvm::BasicBlock &LoopBlock = *std::next(Function.begin());
  llvm::BasicBlock &ExitBlock = Function.back();
  llvm::Instruction &Call = Block.front();
  llvm::Instruction &Read = *std::next(Block.begin());
  llvm::Instruction &Redundant = LoopBlock.front();
  llvm::Instruction &Write = ExitBlock.front();
  llvm::Instruction &ReadWrite = *std::next(ExitBlock.begin());
  const llvm::DataLayout &DL = Module->getDataLayout();
  unsigned PredicateCalls = 0;
  auto CanInstrument = [&](const Observation &Obs) {
    ++PredicateCalls;
    EXPECT_FALSE(Obs.isCallLike());
    return Obs.getInstructions().size() == 1;
  };

  for (bool Reversed : {false, true}) {
    SCOPED_TRACE(Reversed);
    MAIRGraph Graph(Function);
    ProgramPoint &Point = Graph.createBBProgramPoint(Block, true);
    auto AddConflict = [&] {
      Point.addObservation(Observation(
          {&Read, &Redundant},
          AbstractAccess(Function.getArg(0), AbstractValue::constant(0),
                         AbstractValue::constant(4)),
          AccessKind::Read));
    };
    if (Reversed)
      AddConflict();
    const Observation &ReadObs =
        Point.addObservation(Observation::fromInstruction(Read, DL));
    const Observation &RedundantObs =
        Point.addObservation(Observation::fromInstruction(Redundant, DL));
    const Observation &WriteObs =
        Point.addObservation(Observation::fromInstruction(Write, DL));
    const Observation &CallObs =
        Point.addObservation(Observation::fromInstruction(Call, DL));
    if (!Reversed)
      AddConflict();
    ASSERT_TRUE(Graph.verify());
    auto Modeled = collectModeledSources(Graph);
    ASSERT_EQ(Modeled.size(), 4u);
    PredicateCalls = 0;
    auto Result =
        selectMaterializableObservations(Graph, Modeled, CanInstrument);
    EXPECT_EQ(PredicateCalls, 4u);
    ASSERT_EQ(Result.Targets.size(), 1u);
    EXPECT_TRUE(Result.Targets.contains(&WriteObs));
    EXPECT_FALSE(Result.Targets.contains(&ReadObs));
    EXPECT_FALSE(Result.Targets.contains(&RedundantObs));
    EXPECT_FALSE(Result.Targets.contains(&CallObs));
    ASSERT_EQ(Result.HandledSources.size(), 2u);
    EXPECT_TRUE(Result.HandledSources.contains(&Write));
    EXPECT_TRUE(Result.HandledSources.contains(&Call));
    EXPECT_FALSE(Result.HandledSources.contains(&Read));
    EXPECT_FALSE(Result.HandledSources.contains(&Redundant));
  }

  {
    MAIRGraph Graph(Function, MAIROptimizationConfig::forAddressSanitizer());
    ProgramPoint &CallPoint = Graph.createInstProgramPoint(Block, Call);
    ProgramPoint &ReadPoint = Graph.createInstProgramPoint(Block, Read);
    ProgramPoint &RedundantPoint =
        Graph.createInstProgramPoint(LoopBlock, Redundant);
    Graph.addEdge(CallPoint, ReadPoint, PPEdgeKind::Dom);
    Graph.addEdge(ReadPoint, RedundantPoint, PPEdgeKind::Dom);
    auto Modeled = collectModeledSources(Graph);
    ASSERT_EQ(Modeled.size(), 3u);
    MAIRGraphAnalysisManager AM;
    EXPECT_FALSE(
        RedundantObservationEliminationPass().run(Graph, AM).areAllPreserved());
    EXPECT_TRUE(ReadPoint.empty());
    EXPECT_TRUE(RedundantPoint.empty());
    ASSERT_EQ(CallPoint.size(), 1u);
    EXPECT_EQ(collectModeledSources(Graph).size(), 1u);
    ASSERT_TRUE(Graph.verify());
    PredicateCalls = 0;
    auto Result =
        selectMaterializableObservations(Graph, Modeled, CanInstrument);
    EXPECT_EQ(PredicateCalls, 0u);
    EXPECT_TRUE(Result.Targets.empty());
    ASSERT_EQ(Result.HandledSources.size(), 3u);
    for (const llvm::Instruction *Source : {&Call, &Read, &Redundant})
      EXPECT_TRUE(Result.HandledSources.contains(Source));
  }

  for (bool InstrumentReads : {false, true}) {
    for (bool InstrumentWrites : {false, true}) {
      SCOPED_TRACE(InstrumentReads);
      SCOPED_TRACE(InstrumentWrites);
      llvm::PassInstrumentationCallbacks PIC;
      llvm::FunctionAnalysisManager FAM;
      registerLoopAnalyses(FAM, PIC);
      MAOpt<> Opt(Function, FAM, &PIC, true,
                  MAIROptimizationConfig::forAddressSanitizer());
      MAIRGraph &Graph = Opt.build();
      ASSERT_EQ(Graph.children().size(), 1u);
      EXPECT_EQ(collectModeledSources(Graph).size(), 5u);
      llvm::SmallPtrSet<const Observation *, 32> Kept;
      for (const ProgramPoint *Point : Graph.allProgramPoints()) {
        for (const Observation &Obs : *Point) {
          if ((!InstrumentReads && Obs.isRead()) ||
              (!InstrumentWrites && Obs.isWrite()))
            continue;
          Kept.insert(&Obs);
        }
      }
      Opt.retainObservations(Kept);
      auto Modeled = collectModeledSources(Graph);
      EXPECT_EQ(Modeled.size(),
                1u + 2u * InstrumentReads + 2u * InstrumentWrites);
      EXPECT_EQ(Modeled.contains(&Read), InstrumentReads);
      EXPECT_EQ(Modeled.contains(&Redundant), InstrumentReads);
      EXPECT_EQ(Modeled.contains(&Write), InstrumentWrites);
      EXPECT_EQ(Modeled.contains(&Call), InstrumentWrites);
      EXPECT_TRUE(Modeled.contains(&ReadWrite));
      PredicateCalls = 0;
      auto Result =
          selectMaterializableObservations(Graph, Modeled, CanInstrument);
      EXPECT_EQ(Result.Targets.size(),
                1u + 2u * InstrumentReads + InstrumentWrites);
      EXPECT_EQ(PredicateCalls, Result.Targets.size());
      EXPECT_EQ(Result.HandledSources.size(), Modeled.size());
      for (const llvm::Instruction *Source : Modeled)
        EXPECT_TRUE(Result.HandledSources.contains(Source));
      for (const Observation *Obs : Kept)
        EXPECT_EQ(Result.Targets.contains(Obs), !Obs->isCallLike());
      EXPECT_TRUE(Graph.verify());
    }
  }
}

TEST(MAIRLoopHierarchy, IrreducibleCyclesStillBuild) {
  llvm::LLVMContext Ctx;
  auto Module = parseModule(R"IR(
define void @f(i1 %entry.side, i1 %again) {
entry:
  br i1 %entry.side, label %left, label %right
left:
  br i1 %again, label %right, label %exit
right:
  br i1 %again, label %left, label %exit
exit:
  ret void
}
)IR",
                            Ctx);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("f");
  MAIRGraph Graph = buildGraph(Function);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
