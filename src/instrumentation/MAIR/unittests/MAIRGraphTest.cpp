#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRBarrierAnalysis.h"
#include "MAIR/MAIRDump.h"
#include "MAIR/MAIRStatLog.h"
#include "MAIR/MAOpt.h"
#include "MAIR/Transforms/CommonObservationMotionPass.h"
#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIRPassTestUtil.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <tuple>
#include <type_traits>
#include <utility>

using namespace mair;

namespace {
static_assert(
    std::is_same_v<decltype(std::declval<Observation &>().getMemoryObject()),
                   MemoryObject &>);
static_assert(std::is_same_v<
              decltype(std::declval<const Observation &>().getMemoryObject()),
              const MemoryObject &>);
static_assert(!std::is_default_constructible_v<ProgramPointSite>);
static_assert(!std::is_default_constructible_v<ProgramPoint>);

bool compareTestObservations(const Observation &LHS, const Observation &RHS) {
  const AbstractAccess *LRegion = LHS.getContiguousRegion();
  const AbstractAccess *RRegion = RHS.getContiguousRegion();
  assert(LRegion && RRegion && LRegion->getOffset().isConstant() &&
         RRegion->getOffset().isConstant() && LRegion->getSize().isConstant() &&
         RRegion->getSize().isConstant());
  if (LRegion->getOffset() != RRegion->getOffset())
    return LRegion->getOffset().getConstant() <
           RRegion->getOffset().getConstant();
  if (LRegion->getSize() != RRegion->getSize())
    return LRegion->getSize().getConstant() > RRegion->getSize().getConstant();
  return LHS.getLevel() > RHS.getLevel();
}

TEST(MAOpt, BuildsOptimizesAndExposesProgramPointObservations) {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M = parseModule(R"(
    define void @f(ptr %p, ptr %q) {
    entry:
      %a = load i32, ptr %p
      %b = load i32, ptr %q
      ret void
    }
  )",
                                                Ctx);
  ASSERT_NE(M, nullptr);
  llvm::Function &F = *M->getFunction("f");

  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::TargetLibraryAnalysis(); });
  FAM.registerPass([] { return llvm::TargetIRAnalysis(); });
  FAM.registerPass([] { return llvm::AssumptionAnalysis(); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  FAM.registerPass([] { return llvm::ScalarEvolutionAnalysis(); });

  MAOpt<> Opt(F, FAM, &PIC, /*KeepFullDebug=*/true);
  MAIRGraph &Graph = Opt.build();
  ASSERT_TRUE(Graph.verify());
  ASSERT_FALSE(Opt.optimize());

  size_t NumObservations = 0;
  for (const ProgramPoint *PP : Opt.getGraph().allProgramPoints()) {
    for (const Observation &Obs : *PP) {
      EXPECT_EQ(Obs.getParent(), PP);
      EXPECT_EQ(Obs.getInstructions().size(), 1u);
      ++NumObservations;
    }
  }
  EXPECT_EQ(NumObservations, 2u);

  MAOpt<> O1(F, FAM, &PIC, /*KeepFullDebug=*/false);
  O1.build();
  ASSERT_FALSE(O1.optimize());
  EXPECT_TRUE(O1.getGraph().verify());
}

TEST(MAOpt, RetainsSelectedObservations) {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M = parseModule(R"(
    define void @f(ptr %p, ptr %q) {
    entry:
      %a = load i32, ptr %p
      %b = load i32, ptr %q
      ret void
    }
  )",
                                                Ctx);
  ASSERT_NE(M, nullptr);
  llvm::Function &F = *M->getFunction("f");
  llvm::Argument *P = F.getArg(0);
  llvm::Argument *Q = F.getArg(1);

  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::TargetLibraryAnalysis(); });
  FAM.registerPass([] { return llvm::TargetIRAnalysis(); });
  FAM.registerPass([] { return llvm::AssumptionAnalysis(); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  FAM.registerPass([] { return llvm::ScalarEvolutionAnalysis(); });

  MAOpt<> Opt(F, FAM, &PIC, /*KeepFullDebug=*/true);
  MAIRGraph &Graph = Opt.build();
  const MemoryObject *PObject = Graph.getMemoryObject(*P);
  const MemoryObject *QObject = Graph.getMemoryObject(*Q);
  ASSERT_NE(PObject, nullptr);
  ASSERT_NE(QObject, nullptr);
  EXPECT_EQ(llvm::range_size(PObject->users()), 1u);
  EXPECT_EQ(llvm::range_size(QObject->users()), 1u);

  ASSERT_FALSE(Opt.optimize());
  const Observation *Target = nullptr;
  for (const ProgramPoint &PP : testProgramPoints(Graph)) {
    if (!PP.empty()) {
      Target = &*PP.begin();
      break;
    }
  }
  ASSERT_NE(Target, nullptr);
  llvm::SmallPtrSet<const Observation *, 1> Targets;
  Targets.insert(Target);
  Opt.retainObservations(Targets);

  size_t NumObservations = 0;
  for (const ProgramPoint &PP : testProgramPoints(Graph))
    NumObservations += PP.size();
  EXPECT_EQ(NumObservations, 1u);
  EXPECT_EQ(llvm::range_size(PObject->users()), 1u);
  EXPECT_EQ(llvm::range_size(QObject->users()), 0u);
  EXPECT_TRUE(Graph.verify());
}

struct UnmatchedBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::FenceInst>(I);
  }

  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::FenceInst>(I);
  }
};

struct AtomicBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return I.isAtomic();
  }

  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return I.isAtomic();
  }
};

struct CountingCallBarrierTrait {
  static inline unsigned ForwardQueries = 0;
  static inline unsigned BackwardQueries = 0;

  static void reset() {
    ForwardQueries = 0;
    BackwardQueries = 0;
  }

  static bool isForwardBarrier(const llvm::Instruction &I) {
    ++ForwardQueries;
    return llvm::isa<llvm::CallBase>(I);
  }

  static bool isBackwardBarrier(const llvm::Instruction &I) {
    ++BackwardQueries;
    return llvm::isa<llvm::CallBase>(I);
  }
};

TEST(MAOpt, AddressSanitizerKeepsAccessAfterLoopLifetimeBarrier) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @free(ptr)
define i32 @f(ptr %p) {
entry:
  store i32 1, ptr %p, align 4
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %value = load i32, ptr %p, align 4
  %first = icmp eq i32 %i, 0
  %released = select i1 %first, ptr %p, ptr null
  call void @free(ptr %released)
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, 2
  br i1 %done, label %exit, label %loop
exit:
  ret i32 %value
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAOpt<CallBarrierTrait> Opt(F, FAM, &PIC, false,
                              MAIROptimizationConfig::forAddressSanitizer());
  Opt.build();
  ASSERT_FALSE(Opt.optimize());

  unsigned LoopLoads = 0;
  for (const ProgramPoint *PP : Opt.getGraph().allProgramPoints())
    for (const Observation &Obs : *PP)
      for (const llvm::Instruction *Inst : Obs.getInstructions())
        if (llvm::isa<llvm::LoadInst>(Inst) &&
            Inst->getParent()->getName() == "loop")
          ++LoopLoads;
  EXPECT_EQ(LoopLoads, 1u);
  EXPECT_TRUE(Opt.getGraph().verify());
}

TEST(MAIRBarrierAnalysis, SummarizesBarrierBlocksAndPaths) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()

define void @f(i1 %c, i32* %p) {
entry:
  br i1 %c, label %then, label %else

then:
  call void @barrier()
  br label %merge

else:
  br label %merge

merge:
  %v = load i32, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRBarrierAnalysis<CallBarrierTrait> Analysis;
  const llvm::BasicBlock &Entry = *F.begin();
  const llvm::BasicBlock &Then = *std::next(F.begin());
  const llvm::BasicBlock &Merge = *std::next(F.begin(), 3);
  EXPECT_TRUE(
      Analysis.hasFwdBarrierBetween(Then, nullptr, Then.getTerminator()));
  EXPECT_FALSE(Analysis.hasFwdBarrierBetween(Merge, &Merge.front(),
                                             Merge.getTerminator()));
  EXPECT_TRUE(Analysis.hasFwdBarrierPath(Entry, Merge));
  EXPECT_FALSE(Analysis.hasBwdBarrierPath(Merge, Entry));
}

TEST(MAIRBarrierAnalysis, LazilyQueriesBarrierTrait) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()

define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  call void @barrier()
  store i32 %v, ptr %p, align 4
  ret void
clean:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::BasicBlock &Clean = *std::next(F.begin());
  auto InstIt = Entry.begin();
  llvm::Instruction *Load = &*InstIt++;
  llvm::Instruction *Call = &*InstIt++;
  llvm::Instruction *Store = &*InstIt++;
  ASSERT_TRUE(llvm::isa<llvm::LoadInst>(Load));
  ASSERT_TRUE(llvm::isa<llvm::CallBase>(Call));
  ASSERT_TRUE(llvm::isa<llvm::StoreInst>(Store));

  CountingCallBarrierTrait::reset();
  MAIRBarrierAnalysis<CountingCallBarrierTrait> Analysis;
  EXPECT_EQ(CountingCallBarrierTrait::ForwardQueries, 0u);
  EXPECT_EQ(CountingCallBarrierTrait::BackwardQueries, 0u);

  EXPECT_TRUE(Analysis.hasFwdBarrierBetween(Entry, Load, Store));
  EXPECT_GT(CountingCallBarrierTrait::ForwardQueries, 0u);
  EXPECT_EQ(CountingCallBarrierTrait::BackwardQueries, 0u);
  unsigned QueriesAfterFirstRange = CountingCallBarrierTrait::ForwardQueries;
  EXPECT_TRUE(Analysis.hasFwdBarrierBetween(Entry, Load, Store));
  EXPECT_GE(CountingCallBarrierTrait::ForwardQueries, QueriesAfterFirstRange);

  unsigned QueriesBeforeCleanBlock = CountingCallBarrierTrait::ForwardQueries;
  EXPECT_FALSE(Analysis.hasFwdBarrier(Clean));
  unsigned QueriesAfterCleanBlock = CountingCallBarrierTrait::ForwardQueries;
  EXPECT_GT(QueriesAfterCleanBlock, QueriesBeforeCleanBlock);
  EXPECT_FALSE(Analysis.hasFwdBarrier(Clean));
  EXPECT_EQ(CountingCallBarrierTrait::ForwardQueries, QueriesAfterCleanBlock);
}
} // namespace

TEST(MAIRGraph, BasicConstructionAndDump) {
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
  const llvm::Instruction &Load = *BB.begin();

  MAIRGraph Graph(F);
  auto &Entry = Graph.createBBProgramPoint(BB, true);
  auto &PP = Graph.createBBProgramPoint(BB, true);
  PP.addObservation(Observation::fromInstruction(Load, M->getDataLayout()));
  ProgramPoint Other("o1", PP.getSite(), PP.getGuard());
  Other.addObservation(Observation::fromInstruction(Load, M->getDataLayout()));
  PP.mergeObservationsFrom(Other);
  Graph.addEdge(Entry, PP, PPEdgeKind::Dom);

  EXPECT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.size(), 2u);
  EXPECT_TRUE(Entry.isBasicBlockEntrySite());
  EXPECT_TRUE(PP.isBasicBlockEntrySite());
  ASSERT_EQ(Entry.children().size(), 1u);
  EXPECT_EQ(&Entry.children()[0].getNode(), &PP);
  EXPECT_EQ(&PP.parents()[0].getNode(), &Entry);
  ASSERT_EQ(PP.observations().size(), 2u);
  for (const Observation &Obs : PP)
    EXPECT_EQ(Obs.getParent(), &PP);

  unsigned ObsCount = 0;
  unsigned AccessCount = 0;
  for (const ProgramPoint &Node : testProgramPoints(Graph)) {
    ++ObsCount;
    for (const Observation &Obs : Node) {
      (void)Obs;
      ++AccessCount;
    }
  }
  EXPECT_EQ(ObsCount, 2u);
  EXPECT_EQ(AccessCount, 2u);

  std::string Dump;
  raw_string_ostream OS(Dump);
  dumpMAIR(Graph, OS);
  OS.flush();
  EXPECT_NE(Dump.find("mair graph of fn @f {"), std::string::npos);
  EXPECT_NE(Dump.find("pp0: preds=[]"), std::string::npos);
  EXPECT_NE(Dump.find("pp1: preds=[pp0(dom)]"), std::string::npos);
  EXPECT_NE(Dump.find("read ptr %p, 4"), std::string::npos);
  EXPECT_NE(Dump.find("succs=[pp1(dom)]"), std::string::npos);
  EXPECT_EQ(Dump.find("site="), std::string::npos);
  EXPECT_EQ(Dump.find("guard="), std::string::npos);
}

TEST(MAIRSchema, EveryProgramPointDerivesItsBlockFromItsAnchor) {
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
  llvm::Instruction &Terminator = *Entry.getTerminator();
  MAIRGraph Graph(F);
  ProgramPoint &EntryPP = Graph.createBBProgramPoint(Entry, true);
  ProgramPoint &MemoryPP = Graph.createInstProgramPoint(Entry, Load);
  ProgramPoint &ExitPP = Graph.createBBProgramPoint(Entry, false);
  ProgramPoint &EdgePP = Graph.createEdgeProgramPoint(Entry, Exit);

  EXPECT_EQ(&EntryPP.getAnchor(), &Load);
  EXPECT_EQ(&MemoryPP.getAnchor(), &Load);
  EXPECT_EQ(&ExitPP.getAnchor(), &Terminator);
  EXPECT_EQ(&EdgePP.getAnchor(), &Terminator);
  for (const ProgramPoint *PP : {&EntryPP, &MemoryPP, &ExitPP, &EdgePP})
    EXPECT_EQ(PP->getBasicBlock(), PP->getAnchor().getParent());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraph, FindsTypedProgramPointAncestors) {
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
  Graph.addEdge(Memory, Exit, PPEdgeKind::Dom);
  Graph.addEdge(Exit, Memory, PPEdgeKind::PostDom);
  Graph.addEdge(Memory, Entry, PPEdgeKind::PostDom);

  EXPECT_TRUE(Graph.isAncestor(Entry, Exit, PPEdgeKind::Dom));
  EXPECT_TRUE(Graph.isAncestor(Exit, Entry, PPEdgeKind::PostDom));
  EXPECT_FALSE(Graph.isAncestor(Entry, Exit, PPEdgeKind::PostDom));
  EXPECT_FALSE(Graph.isAncestor(Exit, Entry, PPEdgeKind::Dom));
  EXPECT_FALSE(Graph.isAncestor(Memory, Memory, PPEdgeKind::Dom));
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraph, AbstractValueConstructsFromInteger) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Value &P = *M->getFunction("f")->arg_begin();

  AbstractValue Zero = 0;
  EXPECT_TRUE(Zero.isConstant());
  EXPECT_EQ(Zero.getConstant(), 0u);
  EXPECT_TRUE(Zero == 0);
  EXPECT_TRUE(0 == Zero);
  EXPECT_FALSE(AbstractValue(4) == 0);
  EXPECT_FALSE(0 == AbstractValue(4));
  EXPECT_FALSE(AbstractValue() == 0);
  EXPECT_FALSE(0 == AbstractValue());
  EXPECT_FALSE(AbstractValue::symbol(P) == 0);
  EXPECT_FALSE(0 == AbstractValue::symbol(P));
  EXPECT_EQ(AbstractValue::constant(0), Zero);
}

TEST(MAIRGraph, AbstractAccessMergesConcreteRanges) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p) {
entry:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Argument &P = *M->getFunction("f")->arg_begin();

  AbstractAccess A(&P, AbstractValue::constant(0), AbstractValue::constant(4));
  AbstractAccess B(&P, AbstractValue::constant(4), AbstractValue::constant(4));
  std::optional<AbstractAccess> Merged = AbstractAccess::merge(A, B);

  ASSERT_TRUE(Merged.has_value());
  EXPECT_EQ(Merged->getBase(), &P);
  EXPECT_EQ(Merged->getOffset(), AbstractValue::constant(0));
  EXPECT_EQ(Merged->getSize(), AbstractValue::constant(8));
  EXPECT_TRUE(Merged->contains(A));
  EXPECT_TRUE(Merged->contains(B));
  EXPECT_FALSE(A.contains(*Merged));
}

TEST(MAIRGraph, GraphTraitsExposeProgramPointGraph) {
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

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ASSERT_TRUE(Graph.verify());

  const ProgramPoint *Entry = Graph.find("pp0");
  const ProgramPoint *Load = Graph.find("pp1");
  const ProgramPoint *Store = Graph.find("pp2");
  const ProgramPoint *Exit = Graph.find("pp3");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  ASSERT_NE(Exit, nullptr);

  llvm::SmallVector<const ProgramPoint *, 4> Nodes;
  for (const ProgramPoint *PP : llvm::nodes<const MAIRGraph *>(&Graph))
    Nodes.push_back(PP);
  ASSERT_EQ(Nodes.size(), 4u);
  EXPECT_EQ(Nodes[0], Entry);
  EXPECT_EQ(Nodes[1], Load);
  EXPECT_EQ(Nodes[2], Store);
  EXPECT_EQ(Nodes[3], Exit);

  llvm::SmallVector<const ProgramPoint *, 2> EntryChildren;
  for (const ProgramPoint *PP : llvm::children<const ProgramPoint *>(Entry))
    EntryChildren.push_back(PP);
  ASSERT_EQ(EntryChildren.size(), 1u);
  EXPECT_EQ(EntryChildren[0], Load);

  llvm::SmallVector<const ProgramPoint *, 2> LoadParents;
  for (const ProgramPoint *PP :
       llvm::inverse_children<const ProgramPoint *>(Load))
    LoadParents.push_back(PP);
  ASSERT_EQ(LoadParents.size(), 2u);
  EXPECT_EQ(LoadParents[0], Entry);
  EXPECT_EQ(LoadParents[1], Store);

  llvm::SmallVector<PPEdgeKind, 2> LoadChildEdgeKinds;
  for (ProgramPointEdge Edge : llvm::children_edges<const ProgramPoint *>(Load))
    LoadChildEdgeKinds.push_back(Edge.getKind());
  ASSERT_EQ(LoadChildEdgeKinds.size(), 2u);
  EXPECT_EQ(LoadChildEdgeKinds[0], PPEdgeKind::PostDom);
  EXPECT_EQ(LoadChildEdgeKinds[1], PPEdgeKind::Dom);

  const Observation *Obs = &Load->observations().front();
  EXPECT_EQ(llvm::GraphTraits<const Observation *>::getEntryNode(Obs), Obs);
  EXPECT_EQ(llvm::GraphTraits<const Observation *>::child_begin(Obs),
            llvm::GraphTraits<const Observation *>::child_end(Obs));
}

TEST(MAIRGraph, TracksObservationsByBasePointer) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, ptr %q) {
entry:
  %pv = load i32, ptr %p, align 4
  store i32 %pv, ptr %p, align 4
  %qv = load i32, ptr %q, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::Argument &P = *F.arg_begin();
  llvm::Argument &Q = *std::next(F.arg_begin());
  MAIRGraph Graph = buildGraph(F);
  ASSERT_TRUE(Graph.verify());

  MemoryObject *PObject = Graph.getMemoryObject(P);
  MemoryObject *QObject = Graph.getMemoryObject(Q);
  ASSERT_NE(PObject, nullptr);
  ASSERT_NE(QObject, nullptr);
  EXPECT_EQ(PObject->getBase(), &P);
  EXPECT_EQ(QObject->getBase(), &Q);
  EXPECT_FALSE(PObject->use_empty());

  unsigned PUses = 0;
  for (MemoryUse &Use : PObject->uses()) {
    Observation *Obs = Use.getUser();
    ASSERT_NE(Obs, nullptr);
    EXPECT_EQ(Use.get(), PObject);
    EXPECT_EQ(Obs->getBase(), &P);
    EXPECT_EQ(&Obs->getMemoryObject(), PObject);
    ++PUses;
  }
  EXPECT_EQ(PUses, 2u);

  unsigned PUsers = 0;
  for (Observation &Obs : PObject->users()) {
    EXPECT_EQ(&Obs.getMemoryObject(), PObject);
    ++PUsers;
  }
  EXPECT_EQ(PUsers, 2u);

  const MAIRGraph &ConstGraph = Graph;
  const MemoryObject *ConstQObject = ConstGraph.getMemoryObject(Q);
  ASSERT_EQ(ConstQObject, QObject);
  unsigned QUses = 0;
  for (const Observation &Obs : ConstQObject->users()) {
    EXPECT_EQ(Obs.getBase(), &Q);
    EXPECT_EQ(&Obs.getMemoryObject(), QObject);
    ++QUses;
  }
  EXPECT_EQ(QUses, 1u);

  ProgramPoint *Store = Graph.find("pp2");
  ASSERT_NE(Store, nullptr);
  Graph.erase(*Store);

  ASSERT_TRUE(Graph.verify());
  PUses = 0;
  for (Observation &Obs : PObject->users()) {
    EXPECT_EQ(Obs.getBase(), &P);
    EXPECT_EQ(&Obs.getMemoryObject(), PObject);
    ++PUses;
  }
  EXPECT_EQ(PUses, 1u);

  ProgramPoint *QLoad = Graph.find("pp3");
  ASSERT_NE(QLoad, nullptr);
  ASSERT_FALSE(QLoad->empty());
  Observation &QAccess = *QLoad->begin();
  QAccess.eraseFromParent();

  ASSERT_TRUE(Graph.verify());
  EXPECT_TRUE(QLoad->empty());
  EXPECT_TRUE(QObject->use_empty());
}

TEST(MAIRGraph, PreservesMemoryObjectUsesAcrossMove) {
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

  llvm::Function &F = *M->getFunction("f");
  llvm::Argument &P = *F.arg_begin();
  MAIRGraph Source = buildGraph(F);
  MemoryObject *ObjectBeforeMove = Source.getMemoryObject(P);
  ASSERT_NE(ObjectBeforeMove, nullptr);

  MAIRGraph Moved(std::move(Source));

  ASSERT_TRUE(Moved.verify());
  MemoryObject *ObjectAfterMove = Moved.getMemoryObject(P);
  ASSERT_EQ(ObjectAfterMove, ObjectBeforeMove);
  unsigned Users = 0;
  for (Observation &Obs : ObjectAfterMove->users()) {
    EXPECT_EQ(&Obs.getMemoryObject(), ObjectAfterMove);
    ++Users;
  }
  EXPECT_EQ(Users, 2u);
}

TEST(MAIRGraph, AbstractAccessParsesConstantGEPOffset) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p) {
entry:
  %gep = getelementptr i32, ptr %p, i64 2
  %v = load i32, ptr %gep, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Function &F = *M->begin();
  const llvm::Argument &P = *F.arg_begin();
  const auto &Load = cast<llvm::LoadInst>(*std::next(F.begin()->begin()));

  Observation Obs = Observation::fromInstruction(Load, M->getDataLayout());

  ASSERT_NE(Obs.getContiguousRegion(), nullptr);
  EXPECT_EQ(Obs.getContiguousRegion()->getBase(), &P);
  EXPECT_EQ(Obs.getContiguousRegion()->getOffset(), AbstractValue::constant(8));
  EXPECT_EQ(Obs.getContiguousRegion()->getSize(), AbstractValue::constant(4));
}

TEST(MAIRGraph, AbstractAccessKeepsSymbolicGEPOffset) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32* %p, i64 %i) {
entry:
  %gep = getelementptr i32, ptr %p, i64 %i
  %v = load i32, ptr %gep, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Function &F = *M->begin();
  const llvm::Argument &P = *F.arg_begin();
  const auto &GEP = *F.begin()->begin();
  const auto &Load = cast<llvm::LoadInst>(*std::next(F.begin()->begin()));

  Observation Obs = Observation::fromInstruction(Load, M->getDataLayout());

  ASSERT_NE(Obs.getContiguousRegion(), nullptr);
  EXPECT_EQ(Obs.getContiguousRegion()->getBase(), &P);
  EXPECT_EQ(Obs.getContiguousRegion()->getOffset(), AbstractValue::symbol(GEP));
  EXPECT_EQ(Obs.getContiguousRegion()->getSize(), AbstractValue::constant(4));
}

TEST(MAIRGraph, EraseProgramPointMaintainsEdges) {
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
  auto &Entry = Graph.createBBProgramPoint(BB, true);
  auto &Load = Graph.createInstProgramPoint(BB, LoadInst);
  Graph.addEdge(Entry, Load, PPEdgeKind::Dom);
  Graph.addEdge(Entry, Load, PPEdgeKind::PostDom);
  Graph.addEdge(Load, Load, PPEdgeKind::Dom);
  Graph.addEdge(Load, Load, PPEdgeKind::PostDom);

  Graph.erase(Load);

  EXPECT_TRUE(Graph.verify());
  EXPECT_EQ(&Graph.getBBEntryRepresentative(BB), &Entry);
  EXPECT_TRUE(Entry.children().empty());
  EXPECT_EQ(Graph.size(), 1u);
}

TEST(MAIRGraph, FunctionLocalProgramPointKindsVerify) {
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
  const llvm::Instruction &Load = *BB.begin();

  MAIRGraph Graph(F);
  auto &Entry = Graph.createBBProgramPoint(BB, true);
  auto &LoadPP = Graph.createInstProgramPoint(BB, Load);
  Graph.addEdge(Entry, LoadPP, PPEdgeKind::Dom);

  EXPECT_TRUE(Graph.verify());
  EXPECT_TRUE(Entry.isBasicBlockEntrySite());
  EXPECT_TRUE(LoadPP.isMemoryInstSite());
  ASSERT_EQ(LoadPP.observations().size(), 1u);
  EXPECT_EQ(LoadPP.observations().front().getParent(), &LoadPP);
  EXPECT_EQ(&Entry.children().front().getNode(), &LoadPP);
}

TEST(MAIRGraph, BuildCreatesLinearMemoryProgramPointChain) {
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

  llvm::Function *FPtr = M->getFunction("f");
  ASSERT_NE(FPtr, nullptr);
  llvm::Function &F = *FPtr;
  MAIRGraph Graph = buildGraph(F);

  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 4u);
  const ProgramPoint *Entry = Graph.find("pp0");
  const ProgramPoint *Load = Graph.find("pp1");
  const ProgramPoint *Store = Graph.find("pp2");
  const ProgramPoint *Exit = Graph.find("pp3");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  ASSERT_NE(Exit, nullptr);
  EXPECT_TRUE(Entry->isBasicBlockEntrySite());
  EXPECT_TRUE(Load->isMemoryInstSite());
  EXPECT_TRUE(Store->isMemoryInstSite());
  EXPECT_TRUE(Exit->isBasicBlockExitSite());
  ASSERT_EQ(Load->observations().size(), 1u);
  ASSERT_EQ(Store->observations().size(), 1u);
  EXPECT_EQ(Load->observations().front().getKind(), AccessKind::Read);
  EXPECT_EQ(Store->observations().front().getKind(), AccessKind::Write);
  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *Load));
  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *Load, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Exit));
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Entry));
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Entry, PPEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Load));
  EXPECT_TRUE(hasSuccessor(Graph, *Exit, *Store));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Exit));
}

TEST(MAIRGraph, CreateFromUsesClosestDominatorAndPostDominatorBoundaries) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i1 %c, i32* %p) {
entry:
  br i1 %c, label %then, label %else

then:
  %tv = load i32, i32* %p, align 4
  br label %merge

else:
  %ev = load i32, i32* %p, align 4
  br label %merge

merge:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function *FPtr = M->getFunction("f");
  ASSERT_NE(FPtr, nullptr);
  llvm::Function &F = *FPtr;
  MAIRGraph Graph = buildGraph(F);

  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Entry = findBlockEntry(Graph, "entry");
  const ProgramPoint *Then = findBlockEntry(Graph, "then");
  const ProgramPoint *Else = findBlockEntry(Graph, "else");
  const ProgramPoint *Merge = findBlockEntry(Graph, "merge");
  const ProgramPoint *EntryExit = findBlockExit(Graph, "entry");
  const ProgramPoint *ThenExit = findBlockExit(Graph, "then");
  const ProgramPoint *ElseExit = findBlockExit(Graph, "else");
  const ProgramPoint *MergeExit = findBlockExit(Graph, "merge");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Then, nullptr);
  ASSERT_NE(Else, nullptr);
  ASSERT_NE(Merge, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  ASSERT_NE(ThenExit, nullptr);
  ASSERT_NE(ElseExit, nullptr);
  ASSERT_NE(MergeExit, nullptr);

  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Then, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Else, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Merge, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Then, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Else, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Merge, PPEdgeKind::Dom));

  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *ThenExit, PPEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *ElseExit, PPEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *EntryExit, PPEdgeKind::PostDom));
  EXPECT_FALSE(hasSuccessor(Graph, *MergeExit, *ThenExit, PPEdgeKind::PostDom));
  EXPECT_FALSE(hasSuccessor(Graph, *MergeExit, *ElseExit, PPEdgeKind::PostDom));
}

TEST(MAIRGraph, CreateFromBlocksForwardBarrierTerminatorAtDominatorExit) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare i32 @__gxx_personality_v0(...)
declare void @barrier()

define void @f() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @barrier() to label %normal unwind label %unwind
normal:
  br label %merge
unwind:
  %lp = landingpad { ptr, i32 } cleanup
  br label %merge
merge:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph<ForwardCallBarrierTrait>(F);
  const ProgramPoint *Entry = findBlockEntry(Graph, "entry");
  const ProgramPoint *EntryExit = findBlockExit(Graph, "entry");
  const ProgramPoint *Normal = findBlockEntry(Graph, "normal");
  const ProgramPoint *Unwind = findBlockEntry(Graph, "unwind");
  const ProgramPoint *Merge = findBlockEntry(Graph, "merge");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  ASSERT_NE(Normal, nullptr);
  ASSERT_NE(Unwind, nullptr);
  ASSERT_NE(Merge, nullptr);

  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *EntryExit, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Normal, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Unwind, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Merge, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *EntryExit, *Normal, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *EntryExit, *Unwind, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *EntryExit, *Merge, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *EntryExit, PPEdgeKind::PostDom));
}

TEST(MAIRGraph, CreateFromBlocksBackwardBarrierTerminatorAtPostDominatorExit) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i32 %x) {
entry:
  callbr void asm "", "r,!i"(i32 %x)
      to label %left [label %right]
left:
  br label %merge
right:
  br label %merge
merge:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph<BackwardCallBarrierTrait>(F);
  const ProgramPoint *Entry = findBlockEntry(Graph, "entry");
  const ProgramPoint *EntryExit = findBlockExit(Graph, "entry");
  const ProgramPoint *Left = findBlockEntry(Graph, "left");
  const ProgramPoint *Right = findBlockEntry(Graph, "right");
  const ProgramPoint *Merge = findBlockEntry(Graph, "merge");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  ASSERT_NE(Left, nullptr);
  ASSERT_NE(Right, nullptr);
  ASSERT_NE(Merge, nullptr);

  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Entry, PPEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Left, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Right, PPEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Merge, PPEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Merge, *EntryExit, PPEdgeKind::PostDom));
}

TEST(MAIRGraph, PropagatesNestedBarrierPastManySiblingLoops) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(ptr %p, i1 %c) {
entry:
  br label %l0
l0:
  %a0 = load i32, ptr %p, align 4
  br i1 %c, label %l0.exit, label %l0
l0.exit:
  br label %l1
l1:
  %a1 = load i32, ptr %p, align 4
  br i1 %c, label %l1.exit, label %l1
l1.exit:
  br label %l2
l2:
  %a2 = load i32, ptr %p, align 4
  br i1 %c, label %l2.exit, label %l2
l2.exit:
  br label %l3
l3:
  %a3 = load i32, ptr %p, align 4
  br i1 %c, label %l3.exit, label %l3
l3.exit:
  br label %l4
l4:
  %a4 = load i32, ptr %p, align 4
  br i1 %c, label %l4.exit, label %l4
l4.exit:
  br label %l5
l5:
  %a5 = load i32, ptr %p, align 4
  br i1 %c, label %l5.exit, label %l5
l5.exit:
  br label %l6
l6:
  %a6 = load i32, ptr %p, align 4
  br i1 %c, label %l6.exit, label %l6
l6.exit:
  br label %outer
outer:
  br label %inner
inner:
  call void @barrier()
  %ai = load i32, ptr %p, align 4
  br i1 %c, label %inner.exit, label %inner
inner.exit:
  br label %outer.latch
outer.latch:
  br i1 %c, label %exit, label %outer
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph<CallBarrierTrait>(F);
  ASSERT_TRUE(Graph.verify());
  // Root plus nine loops used to overflow graphs()' inline storage and crash
  // while folding barriers. The call is only in the inner loop.
  EXPECT_EQ(Graph.graphs().size(), 10u);

  auto LoopScope = [&](llvm::StringRef Header) -> const MAIRGraph * {
    for (const MAIRGraph *Scope : Graph.graphs())
      if (const llvm::Loop *Loop = Scope->getLoop())
        if (Loop->getHeader()->getName() == Header)
          return Scope;
    return nullptr;
  };
  const MAIRGraph *Sibling = LoopScope("l0");
  const MAIRGraph *Outer = LoopScope("outer");
  const MAIRGraph *Inner = LoopScope("inner");
  ASSERT_NE(Sibling, nullptr);
  ASSERT_NE(Outer, nullptr);
  ASSERT_NE(Inner, nullptr);
  EXPECT_FALSE(Sibling->hasForwardBarrier());
  EXPECT_FALSE(Sibling->hasBackwardBarrier());
  EXPECT_TRUE(Inner->hasForwardBarrier());
  EXPECT_TRUE(Inner->hasBackwardBarrier());
  EXPECT_TRUE(Outer->hasForwardBarrier());
  EXPECT_TRUE(Outer->hasBackwardBarrier());
  EXPECT_TRUE(Graph.hasForwardBarrier());
  EXPECT_TRUE(Graph.hasBackwardBarrier());
}

TEST(MAIRGraph, CreateFromHonorsBarrierTraitInsideBlock) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  call void @barrier()
  store i32 %v, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function *FPtr = M->getFunction("f");
  ASSERT_NE(FPtr, nullptr);
  llvm::Function &F = *FPtr;
  MAIRGraph Graph = buildGraph<CallBarrierTrait>(F);

  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Load = Graph.find("pp1");
  const ProgramPoint *Store = Graph.find("pp2");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  EXPECT_FALSE(hasSuccessor(Graph, *Load, *Store));
  EXPECT_FALSE(hasSuccessor(Graph, *Store, *Load));
}

TEST(MAIRGraph, CreateFromOnlyBlocksBarrierCrossingInterval) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  call void @barrier()
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function *FPtr = M->getFunction("f");
  ASSERT_NE(FPtr, nullptr);
  llvm::Function &F = *FPtr;
  MAIRGraph Graph = buildGraph<CallBarrierTrait>(F);

  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Load = Graph.find("pp1");
  const ProgramPoint *Store = Graph.find("pp2");
  const ProgramPoint *Exit = Graph.find("pp3");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  ASSERT_NE(Exit, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Load));
  EXPECT_FALSE(hasSuccessor(Graph, *Store, *Exit));
}

TEST(MAIRGraph, CreateFromKeepsEdgesWhenBarrierTraitDoesNotMatch) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @not_a_barrier()

define void @f(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  call void @not_a_barrier()
  store i32 %v, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function *FPtr = M->getFunction("f");
  ASSERT_NE(FPtr, nullptr);
  llvm::Function &F = *FPtr;
  MAIRGraph Graph = buildGraph<UnmatchedBarrierTrait>(F);

  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Load = Graph.find("pp1");
  const ProgramPoint *Store = Graph.find("pp2");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Load));
}

TEST(MAIRGraph, ModelsObservationPropertiesAndMemoryIntrinsics) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memcpy.element.unordered.atomic.p0.p0.i64(ptr, ptr, i64, i32 immarg)
declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, <4 x i1>, <4 x i32>)
declare void @llvm.masked.store.v4i32.p0(<4 x i32>, ptr, <4 x i1>)

define void @f(ptr %p, ptr %q, i64 %size, <4 x i1> %mask,
               <4 x i32> %values) {
entry:
  %volatile_load = load volatile i32, ptr %p, align 4
  %atomic_load = load atomic i32, ptr %p monotonic, align 4
  %rmw = atomicrmw add ptr %p, i32 1 monotonic
  %cas = cmpxchg ptr %p, i32 0, i32 1 monotonic monotonic
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %size, i1 false)
  call void @llvm.memcpy.p0.p0.i64(ptr %p, ptr %q, i64 %size, i1 true)
  call void @llvm.memmove.p0.p0.i64(ptr %q, ptr %p, i64 %size, i1 false)
  call void @llvm.memcpy.element.unordered.atomic.p0.p0.i64(
      ptr %p, ptr %q, i64 %size, i32 4)
  %masked_load = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> %mask, <4 x i32> zeroinitializer)
  call void @llvm.masked.store.v4i32.p0(
      <4 x i32> %values, ptr %q, <4 x i1> %mask)
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ASSERT_TRUE(Graph.verify());

  unsigned NumProgramPoints = 0;
  unsigned NumObservations = 0;
  unsigned NumAtomic = 0;
  unsigned NumVolatile = 0;
  unsigned NumCallLike = 0;
  unsigned NumMasked = 0;
  const ProgramPoint *Memcpy = nullptr;
  for (const ProgramPoint &PP : testProgramPoints(Graph)) {
    if (!PP.isMemoryInstSite())
      continue;
    ++NumProgramPoints;
    NumObservations += PP.size();
    if (const auto *MI = llvm::dyn_cast<llvm::MemCpyInst>(&PP.getAnchor()))
      Memcpy = &PP;
    for (const Observation &Obs : PP) {
      NumAtomic += Obs.isAtomic();
      NumVolatile += Obs.isVolatile();
      NumCallLike += Obs.isCallLike();
      NumMasked += Obs.isMasked();
    }
  }

  EXPECT_EQ(NumProgramPoints, 10u);
  EXPECT_EQ(NumObservations, 13u);
  EXPECT_EQ(NumAtomic, 5u);
  EXPECT_EQ(NumVolatile, 3u);
  EXPECT_EQ(NumCallLike, 7u);
  EXPECT_EQ(NumMasked, 2u);

  ASSERT_NE(Memcpy, nullptr);
  ASSERT_EQ(Memcpy->size(), 2u);
  auto It = Memcpy->begin();
  const Observation &Dest = *It++;
  const Observation &Source = *It;
  EXPECT_TRUE(Dest.isWrite());
  EXPECT_EQ(Dest.getBase(), F.getArg(0));
  EXPECT_TRUE(Source.isRead());
  EXPECT_EQ(Source.getBase(), F.getArg(1));
  EXPECT_TRUE(Dest.getContiguousRegion()->getSize().isSymbol());
  EXPECT_EQ(Dest.getContiguousRegion()->getSize().getSymbol(), F.getArg(2));
}

TEST(MAIRGraph, MemoryIntrinsicsAreAccessesRatherThanCallBarriers) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)

define void @f(ptr %p, ptr %q) {
entry:
  %before = load i8, ptr %p
  call void @llvm.memcpy.p0.p0.i64(ptr %p, ptr %q, i64 4, i1 false)
  store i8 %before, ptr %q
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph<CallBarrierTrait>(F);
  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Load = Graph.find("pp1");
  const ProgramPoint *Memcpy = Graph.find("pp2");
  const ProgramPoint *Store = Graph.find("pp3");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Memcpy, nullptr);
  ASSERT_NE(Store, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Memcpy));
  EXPECT_TRUE(hasSuccessor(Graph, *Memcpy, *Load));
  EXPECT_TRUE(hasSuccessor(Graph, *Memcpy, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Memcpy));
}

TEST(MAIRGraph, AtomicObservationProgramPointRemainsABarrierEndpoint) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %before = load i32, ptr %p
  %atomic = load atomic i32, ptr %p acquire, align 4
  store i32 %before, ptr %p
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph<AtomicBarrierTrait>(F);
  ASSERT_TRUE(Graph.verify());
  const ProgramPoint *Load = Graph.find("pp1");
  const ProgramPoint *Atomic = Graph.find("pp2");
  const ProgramPoint *Store = Graph.find("pp3");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Atomic, nullptr);
  ASSERT_NE(Store, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Atomic));
  EXPECT_TRUE(hasSuccessor(Graph, *Atomic, *Load));
  EXPECT_FALSE(hasSuccessor(Graph, *Atomic, *Store));
  EXPECT_FALSE(hasSuccessor(Graph, *Store, *Atomic));
}

TEST(MAIRGraphPass, SpecialObservationsRemainAtTheirOriginalProgramPoint) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)

define void @f(ptr %p) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 4, i1 false)
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  ProgramPoint *Memset = Graph.find("pp1");
  ASSERT_NE(Memset, nullptr);
  ASSERT_EQ(Memset->size(), 1u);
  EXPECT_TRUE(Memset->begin()->isCallLike());

  MAIRGraphAnalysisManager AM;
  llvm::PassInstrumentationCallbacks PIC;
  registerMAIRGraphAnalyses(AM, &PIC);
  SimplifyGraphPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.find("pp1"), Memset);
  ASSERT_EQ(Memset->size(), 1u);
  EXPECT_TRUE(Memset->begin()->isCallLike());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraph, CreateFromHandlesFunctionWithoutObservations) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define i32 @f(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function *FPtr = M->getFunction("f");
  ASSERT_NE(FPtr, nullptr);
  llvm::Function &F = *FPtr;
  MAIRGraph Graph = buildGraph(F);

  EXPECT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 2u);
  const ProgramPoint *Entry = Graph.find("pp0");
  const ProgramPoint *Exit = Graph.find("pp1");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Exit, nullptr);
  EXPECT_TRUE(Entry->isBasicBlockEntrySite());
  EXPECT_TRUE(Exit->isBasicBlockExitSite());
  EXPECT_TRUE(Entry->observations().empty());
}

TEST(MAIRGraph, CreateFromFAMMatchesExplicitAnalysisEntry) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i1 %c, i32* %p) {
entry:
  br i1 %c, label %then, label %merge

then:
  %v = load i32, i32* %p, align 4
  br label %merge

merge:
  store i32 0, i32* %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function *FPtr = M->getFunction("f");
  ASSERT_NE(FPtr, nullptr);
  llvm::Function &F = *FPtr;
  MAIRGraph Explicit = buildGraph(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  FAM.registerPass([] { return llvm::AssumptionAnalysis(); });
  FAM.registerPass([] { return llvm::TargetIRAnalysis(); });
  const llvm::DominatorTree &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(F);
  const llvm::PostDominatorTree &PDT =
      FAM.getResult<llvm::PostDominatorTreeAnalysis>(F);
  const llvm::LoopInfo &LI = FAM.getResult<llvm::LoopAnalysis>(F);
  MAIRGraph FromFAM = createGraphWithFunctionAnalyses(F, FAM);

  ASSERT_TRUE(Explicit.verify());
  ASSERT_TRUE(FromFAM.verify());
  EXPECT_EQ(FromFAM.toString(), Explicit.toString());
  EXPECT_EQ(&FromFAM.getDominatorTree(), &DT);
  EXPECT_EQ(&FromFAM.getPostDominatorTree(), &PDT);
  EXPECT_EQ(&FromFAM.getLoopInfo(), &LI);
  EXPECT_NE(Explicit.getDominatorTree().getNode(&F.getEntryBlock()), nullptr);
  EXPECT_NE(Explicit.getPostDominatorTree().getNode(&F.getEntryBlock()),
            nullptr);
  EXPECT_EQ(Explicit.getLoopInfo().getLoopDepth(&F.getEntryBlock()), 0u);

  const llvm::DominatorTree *OwnedDT = &Explicit.getDominatorTree();
  const llvm::PostDominatorTree *OwnedPDT = &Explicit.getPostDominatorTree();
  const llvm::LoopInfo *OwnedLI = &Explicit.getLoopInfo();
  MAIRGraph Moved(std::move(Explicit));
  EXPECT_EQ(&Moved.getDominatorTree(), OwnedDT);
  EXPECT_EQ(&Moved.getPostDominatorTree(), OwnedPDT);
  EXPECT_EQ(&Moved.getLoopInfo(), OwnedLI);
  MAIRGraph Assigned;
  Assigned = std::move(Moved);
  EXPECT_EQ(&Assigned.getDominatorTree(), OwnedDT);
  EXPECT_EQ(&Assigned.getPostDominatorTree(), OwnedPDT);
  EXPECT_EQ(&Assigned.getLoopInfo(), OwnedLI);
}

TEST(MAIRLoopAnalysis, MapsProgramPointsToLoopsAndRepresentatives) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  %v = load i32, ptr %p, align 4
  %next = add i32 %i, 1
  br label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);
  ASSERT_TRUE(Loops.isAvailable());

  const ProgramPoint *BodyPP = findMemoryProgramPoint(Graph, "body");
  ASSERT_NE(BodyPP, nullptr);
  ASSERT_EQ(BodyPP->size(), 1u);
  llvm::Loop *L = Loops.getLoopFor(*BodyPP);
  ASSERT_NE(L, nullptr);
  EXPECT_EQ(L->getHeader()->getName(), "header");
  EXPECT_TRUE(Loops.isLoopInvariant(*BodyPP->begin(), *L));
  EXPECT_EQ(Loops.getPreheaderExit(*L), findBlockExit(Graph, "entry"));
  EXPECT_EQ(Loops.getUniqueExitEntry(*L), findBlockEntry(Graph, "exit"));
}

TEST(MAIRLoopAnalysis, RejectsScalarEvolutionFromDifferentLoopInfo) {
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

  llvm::Function &F = *M->begin();
  MAIRGraph Graph = buildGraph(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRLoopInfo Loops(Graph, FAM);

  EXPECT_DEATH_IF_SUPPORTED((void)Loops.getSE(), "share LoopInfo");
}

TEST(MAIRLoopAnalysis, RejectsBranchOnlyAccessAsEveryIteration) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %n, i1 %cond) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  br i1 %cond, label %then, label %latch
then:
  %v = load i32, ptr %p, align 4
  br label %latch
latch:
  %next = add i32 %i, 1
  br label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);

  const ProgramPoint *ThenPP = findMemoryProgramPoint(Graph, "then");
  ASSERT_NE(ThenPP, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*ThenPP);
  ASSERT_NE(L, nullptr);
}

TEST(MAIRLoopAnalysis, FindsTopInvariantLoop) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %outer.header
outer.header:
  %oi = phi i32 [ 0, %entry ], [ %oi.next, %outer.latch ]
  %outer.ptr = getelementptr i8, ptr %p, i32 %oi
  br label %inner.header
inner.header:
  %ii = phi i32 [ 0, %outer.header ], [ %ii.next, %inner.header ]
  %inner.ptr = getelementptr i8, ptr %p, i32 %ii
  %outermost = load i8, ptr %p, align 1
  %inner.only = load i8, ptr %outer.ptr, align 1
  %variant = load i8, ptr %inner.ptr, align 1
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

  llvm::Function &F = *M->begin();
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);

  const Observation *OutermostAccess = nullptr;
  const Observation *InnerOnlyAccess = nullptr;
  const Observation *VariantAccess = nullptr;
  for (const ProgramPoint &PP : testProgramPoints(Graph))
    for (const Observation &Obs : PP)
      for (const llvm::Instruction *I : Obs.getInstructions()) {
        if (I->getName() == "outermost")
          OutermostAccess = &Obs;
        else if (I->getName() == "inner.only")
          InnerOnlyAccess = &Obs;
        else if (I->getName() == "variant")
          VariantAccess = &Obs;
      }
  ASSERT_NE(OutermostAccess, nullptr);
  ASSERT_NE(InnerOnlyAccess, nullptr);
  ASSERT_NE(VariantAccess, nullptr);

  llvm::Loop *Inner = Loops.getLoopFor(*OutermostAccess->getParent());
  ASSERT_NE(Inner, nullptr);
  llvm::Loop *Outer = Inner->getParentLoop();
  ASSERT_NE(Outer, nullptr);
  EXPECT_EQ(Loops.getTopInvariantLoop(*OutermostAccess, *Inner), Outer);
  EXPECT_EQ(Loops.getTopInvariantLoop(*InnerOnlyAccess, *Inner), Inner);
  EXPECT_EQ(Loops.getTopInvariantLoop(*VariantAccess, *Inner), nullptr);
  EXPECT_TRUE(Loops.executesOncePerRelevantIteration(
      *OutermostAccess->getParent(), *Inner));
  EXPECT_FALSE(Loops.executesOncePerRelevantIteration(
      *OutermostAccess->getParent(), *Outer));
}

TEST(MAIRLoopAnalysis, RejectsNonLoopAndMultiExitProgramPoints) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %n, i1 %early) {
entry:
  %outside = load i32, ptr %p, align 4
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %inside = load i32, ptr %p, align 4
  br i1 %early, label %exit.early, label %body
body:
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit.normal, label %header
exit.early:
  ret void
exit.normal:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);

  const ProgramPoint *Outside = findMemoryProgramPoint(Graph, "entry");
  ASSERT_NE(Outside, nullptr);
  EXPECT_EQ(Loops.getLoopFor(*Outside), nullptr);

  const ProgramPoint *Inside = findMemoryProgramPoint(Graph, "header");
  ASSERT_NE(Inside, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*Inside);
  ASSERT_NE(L, nullptr);
}

TEST(MAIRLoopAnalysis, ComputesExactProgramPointExecutionCounts) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %body ]
  %header.value = load i8, ptr %p, align 1
  %more = icmp ult i64 %i, 4
  br i1 %more, label %body, label %exit
body:
  %body.value = load i8, ptr %p, align 1
  %next = add i64 %i, 1
  br label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  MAIRLoopInfo Loops(Graph, FAM);
  const ProgramPoint *Header = findMemoryProgramPoint(Graph, "header");
  const ProgramPoint *Body = findMemoryProgramPoint(Graph, "body");
  ASSERT_NE(Header, nullptr);
  ASSERT_NE(Body, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*Header);
  ASSERT_NE(L, nullptr);

  EXPECT_TRUE(Loops.executesOncePerRelevantIteration(*Header, *L));
  EXPECT_TRUE(Loops.executesOncePerRelevantIteration(*Body, *L));
  const llvm::SCEV *HeaderCount = Loops.getExactExecutionCountSCEV(*Header, *L);
  const llvm::SCEV *BodyCount = Loops.getExactExecutionCountSCEV(*Body, *L);
  const auto *HeaderConstant = llvm::dyn_cast<llvm::SCEVConstant>(HeaderCount);
  const auto *BodyConstant = llvm::dyn_cast<llvm::SCEVConstant>(BodyCount);
  ASSERT_NE(HeaderConstant, nullptr);
  ASSERT_NE(BodyConstant, nullptr);
  EXPECT_EQ(HeaderConstant->getAPInt(), llvm::APInt(65, 5));
  EXPECT_EQ(BodyConstant->getAPInt(), llvm::APInt(65, 4));
}

TEST(MAIRLoopAnalysis, BuildsIrreducibleRepeatedSite) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(i1 %entry.sel, i1 %cycle.sel, i1 %again) {
entry:
  br label %header
header:
  br i1 %again, label %dispatch, label %exit
dispatch:
  br i1 %entry.sel, label %a, label %b
a:
  br i1 %cycle.sel, label %b, label %latch
b:
  br i1 %cycle.sel, label %a, label %latch
latch:
  br label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRLoopAnalysis, WidensMaximumBackedgeCountWithoutWrapping) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %value = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 0
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  MAIRLoopInfo Loops(Graph, FAM);
  const ProgramPoint *Header = findMemoryProgramPoint(Graph, "header");
  ASSERT_NE(Header, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*Header);
  ASSERT_NE(L, nullptr);

  const auto *Count = llvm::dyn_cast_or_null<llvm::SCEVConstant>(
      Loops.getExactExecutionCountSCEV(*Header, *L));
  ASSERT_NE(Count, nullptr);
  EXPECT_EQ(Count->getAPInt(), llvm::APInt::getOneBitSet(65, 64));
}

TEST(MAIRLoopAnalysis, DescribesCanonicalizedDedicatedUniqueExits) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @dedicated(i1 %done) {
entry:
  br label %header
header:
  br i1 %done, label %exit, label %header
exit:
  ret void
}
define void @shared(i1 %done, i1 %bypass) {
entry:
  br i1 %bypass, label %exit, label %preheader
preheader:
  br label %header
header:
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  auto Check = [&](llvm::Function &F) {
    llvm::PassInstrumentationCallbacks PIC;
    llvm::FunctionAnalysisManager FAM;
    registerLoopAnalyses(FAM, PIC);
    MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
    MAIRLoopInfo Loops(Graph, FAM);
    const ProgramPoint *Header = findBlockEntry(Graph, "header");
    ASSERT_NE(Header, nullptr);
    llvm::Loop *L = Loops.getLoopFor(*Header);
    ASSERT_NE(L, nullptr);
    std::optional<MAIRLoopExit> Exit = Loops.describeUniqueExit(*L);
    ASSERT_TRUE(Exit.has_value());
    EXPECT_EQ(Exit->Exiting->getName(), "header");
    EXPECT_EQ(Exit->Exit, L->getUniqueExitBlock());
    EXPECT_TRUE(Exit->IsDedicated);
  };

  Check(*M->getFunction("dedicated"));
  Check(*M->getFunction("shared"));
}

TEST(MAIRLoopAnalysis, ChecksValueAvailabilityAtAnchoredAndEdgeSites) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %choose.left) {
entry:
  br i1 %choose.left, label %left, label %right
left:
  %left.value = ptrtoint ptr %p to i64
  br label %merge
right:
  br label %merge
merge:
  %merged = phi i64 [ %left.value, %left ], [ 0, %right ]
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(F, FAM);
  MAIRLoopInfo Loops(Graph, FAM);

  llvm::BasicBlock *Left = nullptr;
  llvm::BasicBlock *Right = nullptr;
  llvm::BasicBlock *Merge = nullptr;
  llvm::Instruction *LeftValue = nullptr;
  llvm::PHINode *MergedValue = nullptr;
  for (llvm::BasicBlock &BB : F) {
    if (BB.getName() == "left") {
      Left = &BB;
      LeftValue = &BB.front();
    } else if (BB.getName() == "right") {
      Right = &BB;
    } else if (BB.getName() == "merge") {
      Merge = &BB;
      MergedValue = llvm::dyn_cast<llvm::PHINode>(&BB.front());
    }
  }
  ASSERT_NE(Left, nullptr);
  ASSERT_NE(Right, nullptr);
  ASSERT_NE(Merge, nullptr);
  ASSERT_NE(LeftValue, nullptr);
  ASSERT_NE(MergedValue, nullptr);
  const ProgramPoint &MergeEntry = Graph.getBBEntryRepresentative(*Merge);

  EXPECT_TRUE(Loops.isValueAvailableAt(*F.getArg(0), MergeEntry.getSite()));
  EXPECT_TRUE(Loops.isValueAvailableAt(*MergedValue, MergeEntry.getSite()));
  EXPECT_FALSE(Loops.isValueAvailableAt(*LeftValue, MergeEntry.getSite()));
  EXPECT_TRUE(Loops.isValueAvailableAt(*LeftValue,
                                       ProgramPointSite::edge(*Left, *Merge)));
  EXPECT_FALSE(Loops.isValueAvailableAt(
      *LeftValue, ProgramPointSite::edge(*Right, *Merge)));
}

TEST(MAIRGraphPass, NestedInnerLifetimeBarrierBlocksObservationExport) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @free(ptr)

define i32 @f(ptr %p) {
entry:
  br label %outer.header
outer.header:
  %oi = phi i32 [ 0, %entry ], [ %onext, %outer.latch ]
  br label %inner.header
inner.header:
  %ii = phi i32 [ 0, %outer.header ], [ %inext, %inner.latch ]
  %invariant = load i32, ptr %p, align 4
  %offset = zext i32 %ii to i64
  %address = getelementptr i8, ptr %p, i64 %offset
  %periodic = load i8, ptr %address, align 1
  %first = icmp eq i32 %ii, 0
  %released = select i1 %first, ptr %p, ptr null
  call void @free(ptr %released)
  %inext = add i32 %ii, 1
  %inner.done = icmp eq i32 %inext, 2
  br i1 %inner.done, label %inner.exit, label %inner.latch
inner.latch:
  br label %inner.header
inner.exit:
  br label %outer.latch
outer.latch:
  %onext = add i32 %oi, 1
  %outer.done = icmp eq i32 %onext, 2
  br i1 %outer.done, label %exit, label %outer.header
exit:
  ret i32 %invariant
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopAnalyses(FAM, PIC);
  MAIRGraph Graph = createGraphWithFunctionAnalyses<CallBarrierTrait>(F, FAM);
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.children().size(), 1u);
  MAIRGraph &Outer = *Graph.children().front();
  ASSERT_EQ(Outer.children().size(), 1u);
  MAIRGraph &Inner = *Outer.children().front();

  EXPECT_TRUE(Inner.hasForwardBarrier());
  EXPECT_TRUE(Inner.hasBackwardBarrier());
  EXPECT_TRUE(Outer.hasForwardBarrier());
  EXPECT_TRUE(Outer.hasBackwardBarrier());
  EXPECT_TRUE(Graph.hasForwardBarrier());
  EXPECT_TRUE(Graph.hasBackwardBarrier());

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  EXPECT_TRUE(
      runPassOnGraphTree(LoopInvariantObservationMotionPass(), Graph, AM)
          .areAllPreserved());
  EXPECT_TRUE(
      runPassOnGraphTree(LoopPeriodicObservationCombinePass(), Graph, AM)
          .areAllPreserved());

  SmallPtrSet<const Instruction *, 2> InnerSources;
  for (const ProgramPoint &PP : testProgramPoints(Graph)) {
    if (!PP.isMemoryInstSite() || !PP.getBasicBlock() ||
        PP.getBasicBlock()->getName() != "inner.header")
      continue;
    for (const Observation &Obs : PP)
      for (const Instruction *Source : Obs.getInstructions())
        if (Source->getName() == "invariant" ||
            Source->getName() == "periodic") {
          EXPECT_EQ(PP.getParent(), &Inner);
          InnerSources.insert(Source);
        }
  }
  EXPECT_EQ(InnerSources.size(), 2u);
  ASSERT_NE(Inner.getLoopPreheader(), nullptr);
  EXPECT_TRUE(Inner.getLoopPreheader()->empty());
  ASSERT_EQ(Inner.getLoopExits().size(), 1u);
  EXPECT_TRUE(Inner.getLoopExits().front()->empty());
  ASSERT_NE(Outer.getLoopPreheader(), nullptr);
  EXPECT_TRUE(Outer.getLoopPreheader()->empty());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraph, TypedEdgeQueriesAndRemovalPreserveOtherKind) {
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

  llvm::Function &F = *M->begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  MAIRGraph Graph(F);
  ProgramPoint &Source = Graph.createInstProgramPoint(BB, BB.front());
  ProgramPoint &Target = Graph.createBBProgramPoint(BB, false);
  Graph.addEdge(Source, Target, PPEdgeKind::Dom);
  Graph.addEdge(Source, Target, PPEdgeKind::PostDom);

  EXPECT_TRUE(Source.hasChild(Target, PPEdgeKind::Dom));
  EXPECT_TRUE(Source.hasChild(Target, PPEdgeKind::PostDom));
  EXPECT_TRUE(Target.hasParent(Source, PPEdgeKind::Dom));
  EXPECT_TRUE(Target.hasParent(Source, PPEdgeKind::PostDom));

  Graph.removeEdge(Source, Target, PPEdgeKind::Dom);
  EXPECT_FALSE(Source.hasChild(Target, PPEdgeKind::Dom));
  EXPECT_TRUE(Source.hasChild(Target, PPEdgeKind::PostDom));
  EXPECT_FALSE(Target.hasParent(Source, PPEdgeKind::Dom));
  EXPECT_TRUE(Target.hasParent(Source, PPEdgeKind::PostDom));
  EXPECT_TRUE(Graph.hasEdge(Source, Target, PPEdgeKind::PostDom));
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraph, EraseClearsBoundaryRepresentativeMaps) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f() {
entry:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  MAIRGraph Graph(F);
  ProgramPoint &OldEntry = Graph.createBBProgramPoint(BB, true);
  ProgramPoint &OldExit = Graph.createBBProgramPoint(BB, false);
  ProgramPoint &Entry = Graph.createBBProgramPoint(BB, true);
  ProgramPoint &Exit = Graph.createBBProgramPoint(BB, false);
  ASSERT_EQ(&Graph.getBBEntryRepresentative(BB), &Entry);
  ASSERT_EQ(&Graph.getBBExitRepresentative(BB), &Exit);
  const MAIRGraph &ConstGraph = Graph;
  ASSERT_EQ(&ConstGraph.getBBEntryRepresentative(BB), &Entry);
  ASSERT_EQ(&ConstGraph.getBBExitRepresentative(BB), &Exit);

  Graph.erase(OldEntry);
  Graph.erase(OldExit);
  EXPECT_EQ(&Graph.getBBEntryRepresentative(BB), &Entry);
  EXPECT_EQ(&Graph.getBBExitRepresentative(BB), &Exit);
  ASSERT_TRUE(Graph.verify());

  Graph.erase(Entry);

#ifndef NDEBUG
  EXPECT_DEATH_IF_SUPPORTED((void)Graph.getBBEntryRepresentative(BB),
                            "Seems the MAIRGraph is broken.");
  EXPECT_DEATH_IF_SUPPORTED((void)ConstGraph.getBBEntryRepresentative(BB),
                            "Seems the MAIRGraph is broken.");
#endif
  Graph.erase(Exit);
#ifndef NDEBUG
  EXPECT_DEATH_IF_SUPPORTED((void)Graph.getBBExitRepresentative(BB),
                            "Seems the MAIRGraph is broken.");
  EXPECT_DEATH_IF_SUPPORTED((void)ConstGraph.getBBExitRepresentative(BB),
                            "Seems the MAIRGraph is broken.");
#endif
  ASSERT_TRUE(Graph.verify());
}

TEST(MAIRGraph, ChainedReplacementsPreserveBoundaryRepresentatives) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f() {
entry:
  br label %middle
middle:
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &FirstBB = F.getEntryBlock();
  llvm::BasicBlock &MiddleBB = *std::next(F.begin());
  llvm::BasicBlock &LastBB = F.back();
  for (bool IsEntry : {true, false}) {
    SCOPED_TRACE(IsEntry);
    MAIRGraph Graph(F);
    ProgramPoint &First = Graph.createBBProgramPoint(FirstBB, IsEntry);
    ProgramPoint &Middle = Graph.createBBProgramPoint(MiddleBB, IsEntry);
    ProgramPoint &Last = Graph.createBBProgramPoint(LastBB, IsEntry);
    llvm::SmallVector<ProgramPoint *, 3> OppositeBoundaries;
    for (llvm::BasicBlock &BB : F)
      OppositeBoundaries.push_back(&Graph.createBBProgramPoint(BB, !IsEntry));
    auto Representative = [&](const llvm::BasicBlock &BB) -> ProgramPoint & {
      return IsEntry ? Graph.getBBEntryRepresentative(BB)
                     : Graph.getBBExitRepresentative(BB);
    };

    Graph.replaceProgramPoint(First, Middle);
    Graph.erase(First);
    EXPECT_EQ(&Representative(FirstBB), &Middle);
    EXPECT_EQ(&Representative(MiddleBB), &Middle);
    EXPECT_EQ(&Representative(LastBB), &Last);
    ASSERT_TRUE(Graph.verify());

    Graph.replaceProgramPoint(Middle, Last);
    Graph.erase(Middle);
    for (llvm::BasicBlock &BB : F) {
      EXPECT_EQ(&Representative(BB), &Last);
      EXPECT_TRUE(Last.representsBlock(BB));
    }
    EXPECT_TRUE(Last.parents().empty());
    EXPECT_TRUE(Last.children().empty());
    ASSERT_TRUE(Graph.verify());

    Graph.erase(Last);
    for (ProgramPoint *Boundary : OppositeBoundaries) {
      const llvm::BasicBlock &BB = *Boundary->getBasicBlock();
      EXPECT_EQ(IsEntry ? &Graph.getBBExitRepresentative(BB)
                        : &Graph.getBBEntryRepresentative(BB),
                Boundary);
    }
    EXPECT_EQ(Graph.size(), 3u);
    EXPECT_TRUE(Graph.verify());
  }
}

TEST(MAIRSchema, EdgeSiteDoesNotReplaceBoundaryRepresentatives) {
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
  ProgramPoint &EntryRep = Graph.createBBProgramPoint(Entry, true);
  ProgramPoint &ExitRep = Graph.createBBProgramPoint(Exit, true);
  ProgramPoint &Edge = Graph.createEdgeProgramPoint(
      Entry, Exit, AnyProvenanceExecutedGuard({&Load}));

  ASSERT_TRUE(Graph.verify());
  EXPECT_TRUE(Edge.isEdgeSite());
  EXPECT_EQ(Edge.getEdgeFrom(), &Entry);
  EXPECT_EQ(Edge.getEdgeTo(), &Exit);
  EXPECT_TRUE(Edge.hasExecutionGuard());
  EXPECT_EQ(&Graph.getBBEntryRepresentative(Entry), &EntryRep);
  EXPECT_EQ(&Graph.getBBEntryRepresentative(Exit), &ExitRep);

  Graph.erase(Edge);
  EXPECT_EQ(&Graph.getBBEntryRepresentative(Entry), &EntryRep);
  EXPECT_EQ(&Graph.getBBEntryRepresentative(Exit), &ExitRep);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, VerifierRejectsCrossFunctionAndNonCFGEdgeSites) {
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
define void @g() {
entry:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::Function &G = *M->getFunction("g");
  llvm::BasicBlock &FEntry = F.getEntryBlock();
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);

  MAIRGraph CrossFunction(F);
  CrossFunction.createEdgeProgramPoint(FEntry, G.getEntryBlock());
  std::string CrossFunctionError;
  llvm::raw_string_ostream CrossFunctionOS(CrossFunctionError);
  EXPECT_FALSE(CrossFunction.verify(&CrossFunctionOS));
  EXPECT_NE(CrossFunctionError.find("edge site crosses functions"),
            std::string::npos);

  MAIRGraph MissingEdge(F);
  MissingEdge.createEdgeProgramPoint(Left, Right);
  std::string MissingEdgeError;
  llvm::raw_string_ostream MissingEdgeOS(MissingEdgeError);
  EXPECT_FALSE(MissingEdge.verify(&MissingEdgeOS));
  EXPECT_NE(MissingEdgeError.find("edge site is not a current CFG edge"),
            std::string::npos);
}

TEST(MAIRSchema, GuardEqualityUsesProvenanceSetSemantics) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, ptr %q) {
entry:
  %a = load i32, ptr %p, align 4
  %b = load i32, ptr %q, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::BasicBlock &BB = M->getFunction("f")->getEntryBlock();
  llvm::Instruction *A = &BB.front();
  llvm::Instruction *B = &*std::next(BB.begin());
  ProgramPointGuard AB = AnyProvenanceExecutedGuard({A, B});
  ProgramPointGuard BA = AnyProvenanceExecutedGuard({B, A});
  ProgramPointGuard AOnly = AnyProvenanceExecutedGuard({A});
  ProgramPointGuard DuplicateA = AnyProvenanceExecutedGuard({A, A});
  ProgramPointGuard ResetAB = AnyProvenanceExecutedGuard({A, B}, &BB);
  ProgramPointGuard ResetBA = AnyProvenanceExecutedGuard({B, A}, &BB);

  EXPECT_EQ(AB, BA);
  EXPECT_NE(AB, AOnly);
  EXPECT_NE(DuplicateA, AB);
  EXPECT_NE(AB, DuplicateA);
  EXPECT_NE(AB, ProgramPointGuard(AlwaysGuard{}));
  EXPECT_EQ(ResetAB, ResetBA);
  EXPECT_NE(ResetAB, AB);
}

TEST(MAIRSchema, VerifierRejectsExecutionGuardResetOutsideItsFunction) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %value = load i32, ptr %p, align 4
  ret void
}
define void @other() {
entry:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &Other = M->getFunction("other")->getEntryBlock();
  MAIRGraph Graph(F);
  Graph.createProgramPoint(
      ProgramPointSite::anchored(*F.getEntryBlock().getTerminator(),
                                 ProgramPoint::SiteKind::BasicBlockExit),
      AnyProvenanceExecutedGuard({&F.getEntryBlock().front()}, &Other));
  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("reset"), std::string::npos);
}

TEST(MAIRSchema, VerifierRejectsExecutionGuardResetOutsideItsProvenancePath) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %take) {
entry:
  br i1 %take, label %access, label %other
access:
  %value = load i32, ptr %p, align 4
  br label %exit
other:
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &Access = *std::next(F.begin());
  llvm::BasicBlock &Other = *std::next(F.begin(), 2);
  llvm::BasicBlock &Exit = *std::next(F.begin(), 3);
  MAIRGraph Graph = buildGraph(F);
  Graph.createProgramPoint(
      ProgramPointSite::anchored(Exit.front(),
                                 ProgramPoint::SiteKind::BasicBlockEntry),
      AnyProvenanceExecutedGuard({&Access.front()}, &Other));
  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("reset"), std::string::npos);
}

TEST(MAIRSchema, VerifierRejectsNonEmptyFunctionlessGraph) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
@base = global i8 0
define void @f() {
entry:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::GlobalVariable &Base = *M->getGlobalVariable("base");
  llvm::Type &I64 = *llvm::Type::getInt64Ty(Ctx);
  MAIRGraph Graph;
  ProgramPoint &PP = Graph.createBBProgramPoint(F.getEntryBlock(), false);
  MAIRScalarExpr Start = Graph.createValueExpr(Base);
  MAIRScalarExpr One = Graph.createConstantExpr(I64, llvm::APInt(64, 1));
  PP.addObservation(Observation({}, PeriodicRegion(&Base, Start, One, One, One),
                                AccessKind::Read));

  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("non-empty graph has no function"), std::string::npos);
}

TEST(MAIRSchema, VerifierRejectsMalformedExecutionGuard) {
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
  llvm::Instruction &Load = F.getEntryBlock().front();
  MAIRGraph Graph(F);
  ProgramPoint &Anchor = Graph.createBBProgramPoint(F.getEntryBlock(), true);
  Graph.createProgramPoint(Anchor.getSite(),
                           AnyProvenanceExecutedGuard({&Load, &Load}));
  Graph.createProgramPoint(
      Anchor.getSite(),
      AnyProvenanceExecutedGuard(llvm::ArrayRef<llvm::Instruction *>()));

  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("execution guard contains duplicate provenance"),
            std::string::npos);
  EXPECT_NE(Error.find("execution guard has empty provenance"),
            std::string::npos);
}

TEST(MAIRSchema, DifferentGuardsCanShareAnAnchoredSite) {
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
  ProgramPoint &Representative = Graph.createBBProgramPoint(BB, true);
  ProgramPoint &Guarded = Graph.createProgramPoint(
      Representative.getSite(), AnyProvenanceExecutedGuard({&Load}));

  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Guarded.getSite(), Representative.getSite());
  EXPECT_NE(Guarded.getGuard(), Representative.getGuard());
  EXPECT_EQ(&Graph.getBBEntryRepresentative(BB), &Representative);

  Graph.erase(Guarded);
  EXPECT_EQ(&Graph.getBBEntryRepresentative(BB), &Representative);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, ProgramPointSortCacheInvalidatesWhenAccessIsAdded) {
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
  llvm::Argument &P = *F.arg_begin();
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);

  auto MakeAccess = [&](uint64_t Offset) {
    return Observation(
        {&Load},
        MemoryRegion(AbstractAccess(&P, AbstractValue::constant(Offset),
                                    AbstractValue::constant(4))),
        AccessKind::Read);
  };
  auto CollectOffsets = [&]() {
    llvm::SmallVector<uint64_t, 4> Offsets;
    for (const Observation &Obs : PP) {
      const AbstractAccess *Region = Obs.getContiguousRegion();
      assert(Region && Region->getOffset().isConstant());
      Offsets.push_back(Region->getOffset().getConstant());
    }
    return Offsets;
  };

  PP.addObservation(MakeAccess(8));
  PP.addObservation(MakeAccess(0));
  PP.sort(compareTestObservations);

  llvm::SmallVector<uint64_t, 4> Offsets = CollectOffsets();
  ASSERT_EQ(Offsets.size(), 2u);
  EXPECT_EQ(Offsets[0], 0u);
  EXPECT_EQ(Offsets[1], 8u);

  PP.sort(compareTestObservations);
  PP.addObservation(MakeAccess(4));
  PP.sort(compareTestObservations);

  Offsets = CollectOffsets();
  ASSERT_EQ(Offsets.size(), 3u);
  EXPECT_EQ(Offsets[0], 0u);
  EXPECT_EQ(Offsets[1], 4u);
  EXPECT_EQ(Offsets[2], 8u);

  auto ReverseCompare = [](const Observation &LHS, const Observation &RHS) {
    return compareTestObservations(RHS, LHS);
  };
  PP.sort(ReverseCompare);
  Offsets = CollectOffsets();
  EXPECT_EQ(Offsets[0], 8u);
  EXPECT_EQ(Offsets[1], 4u);
  EXPECT_EQ(Offsets[2], 0u);
  PP.sort(compareTestObservations);
  Offsets = CollectOffsets();
  EXPECT_EQ(Offsets[0], 0u);
  EXPECT_EQ(Offsets[1], 4u);
  EXPECT_EQ(Offsets[2], 8u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, ProgramPointSortCacheInvalidatesWhenAccessKeyChanges) {
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
  llvm::Argument &P = *F.arg_begin();
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);

  auto MakeAccess = [&](uint64_t Offset, uint64_t Size) {
    return Observation(
        {&Load},
        MemoryRegion(AbstractAccess(&P, AbstractValue::constant(Offset),
                                    AbstractValue::constant(Size))),
        AccessKind::Read);
  };

  Observation &Left = PP.addObservation(MakeAccess(0, 4));
  Observation &Right = PP.addObservation(MakeAccess(4, 4));
  PP.sort(compareTestObservations);
  ASSERT_EQ(&*PP.begin(), &Left);
  ASSERT_TRUE(Right.mergeWith(Left));

  PP.sort(compareTestObservations);
  EXPECT_EQ(&*PP.begin(), &Right);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, ProgramPointSortCacheInvalidatesWhenAccessLevelChanges) {
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
  llvm::Argument &P = *F.arg_begin();
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);
  AbstractAccess Region(&P, AbstractValue::constant(0),
                        AbstractValue::constant(4));

  Observation &Read =
      PP.addObservation(Observation({&Load}, Region, AccessKind::Read));
  Observation &Write =
      PP.addObservation(Observation({&Load}, Region, AccessKind::Write));
  PP.sort(compareTestObservations);
  ASSERT_EQ(&*PP.begin(), &Write);

  Read.mergeKind(AccessKind::Write);
  PP.sort(compareTestObservations);
  EXPECT_EQ(&*PP.begin(), &Read);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, PeriodicRegionAcceptsZeroTripAndSurvivesGraphMove) {
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
  llvm::Argument &P = *F.arg_begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &Load = BB.front();
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);
  MAIRScalarExpr Start = Graph.createValueExpr(P);
  MAIRScalarExpr Stride = Graph.createConstantExpr(*llvm::Type::getInt64Ty(Ctx),
                                                   llvm::APInt(64, 8));
  MAIRScalarExpr AccessSize = Graph.createConstantExpr(
      *llvm::Type::getInt64Ty(Ctx), llvm::APInt(64, 4));
  MAIRScalarExpr ZeroTrips = Graph.createConstantExpr(
      *llvm::Type::getInt64Ty(Ctx), llvm::APInt(64, 0));
  PeriodicRegion Region(&P, Start, Stride, AccessSize, ZeroTrips);
  PP.addObservation(
      Observation({&Load}, MemoryRegion(Region), AccessKind::Read));

  ASSERT_TRUE(Graph.verify());
  ASSERT_TRUE(PP.begin()->isPeriodic());
  ASSERT_NE(PP.begin()->getPeriodicRegion(), nullptr);
  EXPECT_EQ(PP.begin()->getBase(), &P);

  const void *StartNode = Start.getOpaqueNode();
  MAIRGraph Moved(std::move(Graph));
  EXPECT_TRUE(Moved.ownsScalarExpr(Start));
  EXPECT_EQ(Start.getOpaqueNode(), StartNode);
  EXPECT_TRUE(Moved.verify());

  MAIRGraph MoveAssigned(F);
  MoveAssigned.createConstantExpr(*llvm::Type::getInt64Ty(Ctx),
                                  llvm::APInt(64, 99));
  MoveAssigned = std::move(Moved);
  EXPECT_TRUE(MoveAssigned.ownsScalarExpr(Start));
  EXPECT_EQ(Start.getOpaqueNode(), StartNode);
  EXPECT_TRUE(MoveAssigned.verify());
}

TEST(MAIRSchema, TypedScalarExpressionDagVerifiesAndPrints) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %n) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::Argument &P = *F.arg_begin();
  llvm::Argument &N = *std::next(F.arg_begin());
  llvm::Instruction &Load = F.getEntryBlock().front();
  llvm::Type &I32 = *llvm::Type::getInt32Ty(Ctx);
  llvm::Type &I64 = *llvm::Type::getInt64Ty(Ctx);
  llvm::Type &PtrTy = *P.getType();
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(F.getEntryBlock(), true);

  MAIRScalarExpr PExpr = Graph.createValueExpr(P);
  MAIRScalarExpr NExpr = Graph.createValueExpr(N);
  MAIRScalarExpr C2 = Graph.createConstantExpr(I64, llvm::APInt(64, 2));
  MAIRScalarExpr C4 = Graph.createConstantExpr(I64, llvm::APInt(64, 4));
  MAIRScalarExpr C8 = Graph.createConstantExpr(I64, llvm::APInt(64, 8));
  MAIRScalarExpr Start =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add, PtrTy, PExpr, C4);
  MAIRScalarExpr Mul =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::Mul, I64, C2, C4);
  MAIRScalarExpr SMin =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::SMin, I64, Mul, C8);
  MAIRScalarExpr UMin =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::UMin, I64, SMin, C8);
  MAIRScalarExpr SMax =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::SMax, I64, UMin, C2);
  MAIRScalarExpr UMax =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::UMax, I64, SMax, C4);
  MAIRScalarExpr Size =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::UDiv, I64, UMax, C2);
  MAIRScalarExpr Truncated =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::Truncate, I32, UMax);
  MAIRScalarExpr Stride =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::SignExtend, I64, Truncated);
  MAIRScalarExpr Trips =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::ZeroExtend, I64, NExpr);
  PP.addObservation(Observation(
      {&Load}, MemoryRegion(PeriodicRegion(&P, Start, Stride, Size, Trips)),
      AccessKind::Read));

  ASSERT_TRUE(Graph.verify());
  std::string Text = Graph.toString();
  EXPECT_NE(Text.find("start=(%p + 4)"), std::string::npos);
  EXPECT_NE(Text.find("stride=sext("), std::string::npos);
  EXPECT_NE(Text.find("access-size="), std::string::npos);
  EXPECT_NE(Text.find("trips=zext(%n to i64)"), std::string::npos);

  for (bool Shared : {false, true}) {
    SCOPED_TRACE(Shared);
    MAIRGraph ExprGraph(F);
    ProgramPoint &ExprPP =
        ExprGraph.createBBProgramPoint(F.getEntryBlock(), true);
    MAIRScalarExpr Expr = ExprGraph.createConstantExpr(I64, llvm::APInt(64, 1));
    for (unsigned Depth = 0; Depth < 10; ++Depth) {
      MAIRScalarExpr Right =
          Shared
              ? Expr
              : ExprGraph.createConstantExpr(I64, llvm::APInt(64, Depth + 2));
      Expr = ExprGraph.createBinaryExpr(MAIRScalarExpr::Kind::Add, I64, Expr,
                                        Right);
    }
    EXPECT_EQ(Expr.operands()[0].getOpaqueNode() ==
                  Expr.operands()[1].getOpaqueNode(),
              Shared);
    AbstractAccess Access(&P, AbstractValue::constant(0),
                          AbstractValue::constant(4));
    Access.setSizeExpr(Expr);
    ExprPP.addObservation(Observation({&Load}, Access, AccessKind::Read));
    if (Shared)
      ExprPP.addObservation(Observation({&Load}, Access, AccessKind::Read));
    EXPECT_TRUE(ExprGraph.verify());
    std::string Error;
    llvm::raw_string_ostream OS(Error);
    EXPECT_TRUE(ExprGraph.verify(&OS));
    EXPECT_TRUE(Error.empty());
  }
}

TEST(MAIRSchema, VerifierRejectsForeignExpressionAndZeroStride) {
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
  llvm::Argument &P = *F.arg_begin();
  llvm::Instruction &Load = F.getEntryBlock().front();
  llvm::Type &I64 = *llvm::Type::getInt64Ty(Ctx);

  MAIRGraph Graph(F);
  MAIRGraph Other(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(F.getEntryBlock(), true);
  MAIRScalarExpr Start = Graph.createValueExpr(P);
  MAIRScalarExpr ZeroStride = Graph.createConstantExpr(I64, llvm::APInt(64, 0));
  MAIRScalarExpr Size = Graph.createConstantExpr(I64, llvm::APInt(64, 4));
  MAIRScalarExpr ForeignTrips =
      Other.createConstantExpr(I64, llvm::APInt(64, 3));
  PeriodicRegion Region(&P, Start, ZeroStride, Size, ForeignTrips);
  PP.addObservation(
      Observation({&Load}, MemoryRegion(Region), AccessKind::Read));

  EXPECT_FALSE(Graph.verify());
  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("scalar expression belongs to another graph"),
            std::string::npos);
  EXPECT_NE(Error.find("periodic region has zero stride"), std::string::npos);

  MAIRGraph Shared(F);
  MAIRScalarExpr Expr = Shared.createBinaryExpr(MAIRScalarExpr::Kind::Add, I64,
                                                ForeignTrips, ForeignTrips);
  AbstractAccess Access(&P, AbstractValue::constant(0),
                        AbstractValue::constant(4));
  Access.setSizeExpr(Expr);
  std::string Expected;
  llvm::raw_string_ostream ExpectedOS(Expected);
  for (bool IsEntry : {true, false}) {
    ProgramPoint &SharedPP =
        Shared.createBBProgramPoint(F.getEntryBlock(), IsEntry);
    const Observation &Obs =
        SharedPP.addObservation(Observation({&Load}, Access, AccessKind::Read));
    for (unsigned Occurrence = 0; Occurrence < 2; ++Occurrence) {
      ExpectedOS << "MAIR verification failed: scalar expression belongs to "
                    "another graph\n  observation: ";
      Obs.print(ExpectedOS);
      ExpectedOS << "\n  program point: ";
      SharedPP.print(ExpectedOS);
      ExpectedOS << "\n";
    }
  }
  EXPECT_FALSE(Shared.verify());
  std::string SharedError;
  llvm::raw_string_ostream SharedOS(SharedError);
  EXPECT_FALSE(Shared.verify(&SharedOS));
  EXPECT_EQ(SharedError, Expected);
  EXPECT_FALSE(Shared.verify());
}

TEST(MAIRSchema, VerifierRejectsIllTypedPeriodicRegionAndZeroSize) {
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
  llvm::Argument &P = *F.arg_begin();
  llvm::Instruction &Load = F.getEntryBlock().front();
  llvm::Type &I64 = *llvm::Type::getInt64Ty(Ctx);
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(F.getEntryBlock(), true);
  MAIRScalarExpr IntegerStart =
      Graph.createConstantExpr(I64, llvm::APInt(64, 0));
  MAIRScalarExpr PointerStride = Graph.createValueExpr(P);
  MAIRScalarExpr ZeroSize = Graph.createConstantExpr(I64, llvm::APInt(64, 0));
  MAIRScalarExpr TripSource = Graph.createConstantExpr(I64, llvm::APInt(64, 2));
  MAIRScalarExpr Trips =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::Truncate, I64, TripSource);
  PP.addObservation(
      Observation({&Load},
                  MemoryRegion(PeriodicRegion(&P, IntegerStart, PointerStride,
                                              ZeroSize, Trips)),
                  AccessKind::Read));

  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("periodic region start is not pointer typed"),
            std::string::npos);
  EXPECT_NE(Error.find("periodic region stride is not integer typed"),
            std::string::npos);
  EXPECT_NE(Error.find("periodic region has zero access size"),
            std::string::npos);
  EXPECT_NE(Error.find("scalar truncate does not narrow"), std::string::npos);
  EXPECT_FALSE(Graph.verify());
}

TEST(MAIRSchema, VerifierRejectsInvalidScalarOperandWithoutCrashing) {
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
  llvm::Argument &P = *F.arg_begin();
  llvm::Instruction &Load = F.getEntryBlock().front();
  llvm::Type &I64 = *llvm::Type::getInt64Ty(Ctx);
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(F.getEntryBlock(), true);
  MAIRScalarExpr Start = Graph.createValueExpr(P);
  MAIRScalarExpr C4 = Graph.createConstantExpr(I64, llvm::APInt(64, 4));
  MAIRScalarExpr InvalidStride = Graph.createBinaryExpr(
      MAIRScalarExpr::Kind::Add, I64, MAIRScalarExpr(), C4);
  PP.addObservation(Observation(
      {&Load}, MemoryRegion(PeriodicRegion(&P, Start, InvalidStride, C4, C4)),
      AccessKind::Read));

  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("invalid scalar expression"), std::string::npos);
  EXPECT_FALSE(Graph.verify());
}

TEST(MAIRSchema, PeriodicMergesPreserveProvenanceAndRejectUnequalRegions) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %a = load i32, ptr %p, align 4
  %b = load i32, ptr %p, align 4
  %c = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::Argument &P = *F.arg_begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction *AInst = &BB.front();
  llvm::Instruction *BInst = &*std::next(BB.begin());
  llvm::Instruction *CInst = &*std::next(BB.begin(), 2);
  llvm::Type &I64 = *llvm::Type::getInt64Ty(Ctx);
  MAIRGraph Graph(F);
  MAIRScalarExpr StartA = Graph.createValueExpr(P);
  MAIRScalarExpr StartB = Graph.createValueExpr(P);
  MAIRScalarExpr StrideA = Graph.createConstantExpr(I64, llvm::APInt(64, 8));
  MAIRScalarExpr StrideB = Graph.createConstantExpr(I64, llvm::APInt(64, 8));
  MAIRScalarExpr SizeA = Graph.createConstantExpr(I64, llvm::APInt(64, 4));
  MAIRScalarExpr SizeB = Graph.createConstantExpr(I64, llvm::APInt(64, 4));
  MAIRScalarExpr TripsA = Graph.createConstantExpr(I64, llvm::APInt(64, 3));
  MAIRScalarExpr TripsB = Graph.createConstantExpr(I64, llvm::APInt(64, 3));
  Observation A(
      {AInst}, MemoryRegion(PeriodicRegion(&P, StartA, StrideA, SizeA, TripsA)),
      AccessKind::Read);
  Observation B(
      {BInst}, MemoryRegion(PeriodicRegion(&P, StartB, StrideB, SizeB, TripsB)),
      AccessKind::Write);

  expectMergeRejectedWithoutMutation(A, B);
  Observation Left({AInst, BInst, AInst}, A.getRegion(), AccessKind::Read);
  Observation Equal({CInst, BInst}, B.getRegion(), AccessKind::Read);
  ASSERT_EQ(Left.getLevel(), Equal.getLevel());
  ASSERT_TRUE(Left.mergeWith(Equal));
  EXPECT_EQ(Left.getInstructions(), (llvm::ArrayRef<llvm::Instruction *>{
                                        AInst, BInst, AInst, CInst, BInst}));
  EXPECT_EQ(Left.getInstruction(), AInst);
  Observation Unequal(
      {CInst, BInst},
      MemoryRegion(PeriodicRegion(&P, StartB, StrideB, SizeB, SizeB)),
      AccessKind::Read);
  ASSERT_EQ(Left.getLevel(), Unequal.getLevel());
  expectMergeRejectedWithoutMutation(Left, Unequal);
  Observation Contiguous({CInst, BInst},
                         AbstractAccess(&P, AbstractValue::constant(0),
                                        AbstractValue::constant(4)),
                         AccessKind::Read);
  expectMergeRejectedWithoutMutation(Left, Contiguous);
  expectMergeRejectedWithoutMutation(Contiguous, Left);
}

TEST(MAIRSchema, MaskednessIsDerivedFromTheRegion) {
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
  llvm::Instruction &Load = F.getEntryBlock().front();
  llvm::SmallBitVector Mask(1, true);
  MaskedRegion Region(AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                                     AbstractValue::constant(4)),
                      std::move(Mask), 4);
  Observation Obs({&Load}, MemoryRegion(std::move(Region)), AccessKind::Read);

  EXPECT_TRUE(Obs.isMasked());
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(F.getEntryBlock(), true);
  Observation &Inserted = PP.addObservation(Obs);
  EXPECT_TRUE(Inserted.isMasked());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, ContiguousMergesKeepCompleteProvenanceOrder) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %wide = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  %narrow = load i32, ptr %p4, align 4
  %extra = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::Argument &P = *F.arg_begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction *Wide = &BB.front();
  llvm::Instruction *Narrow = &*std::next(BB.begin(), 2);
  llvm::Instruction *Extra = &*std::next(BB.begin(), 3);
  struct MergeCase {
    uint64_t LeftOffset, LeftSize, RightOffset, RightSize;
    bool Prepend;
  };
  for (const MergeCase &Case : {MergeCase{0, 8, 4, 4, false},
                                {4, 4, 0, 8, true},
                                {0, 8, 0, 8, false},
                                {0, 4, 4, 4, false},
                                {4, 4, 0, 4, false},
                                {0, 8, 4, 8, false}}) {
    SCOPED_TRACE(::testing::Message()
                 << Case.LeftOffset << ':' << Case.LeftSize << " + "
                 << Case.RightOffset << ':' << Case.RightSize);
    Observation Left({Wide, Narrow, Wide},
                     AbstractAccess(&P,
                                    AbstractValue::constant(Case.LeftOffset),
                                    AbstractValue::constant(Case.LeftSize)),
                     AccessKind::Read);
    Observation Right({Extra, Narrow},
                      AbstractAccess(&P,
                                     AbstractValue::constant(Case.RightOffset),
                                     AbstractValue::constant(Case.RightSize)),
                      AccessKind::Read);
    ASSERT_TRUE(Left.mergeWith(Right));
    llvm::SmallVector<llvm::Instruction *, 5> Expected{Wide, Narrow, Wide,
                                                       Extra, Narrow};
    if (Case.Prepend)
      Expected = {Extra, Narrow, Wide, Narrow, Wide};
    EXPECT_EQ(Left.getInstructions(), llvm::ArrayRef(Expected));
    EXPECT_EQ(Left.getInstruction(), Expected.front());
    uint64_t Begin = std::min(Case.LeftOffset, Case.RightOffset);
    uint64_t End = std::max(Case.LeftOffset + Case.LeftSize,
                            Case.RightOffset + Case.RightSize);
    EXPECT_EQ(Left.getRegion(), MemoryRegion(AbstractAccess(
                                    &P, AbstractValue::constant(Begin),
                                    AbstractValue::constant(End - Begin))));
  }
}

TEST(MAIRSchema, NewSemanticsHaveExplicitStableDumpSyntax) {
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
  Graph.createBBProgramPoint(Entry, true);
  Graph.createEdgeProgramPoint(Entry, Exit,
                               AnyProvenanceExecutedGuard({&Load}));

  std::string Text = Graph.toString();
  EXPECT_NE(Text.find("site=edge(%entry -> %exit)"), std::string::npos);
  EXPECT_NE(Text.find("guard=any(%v)"), std::string::npos);

  std::string LLVMText;
  llvm::raw_string_ostream LLVMOS(LLVMText);
  dumpLLVM(Graph, LLVMOS);
  LLVMOS.flush();
  EXPECT_NE(LLVMText.find("site=edge(%entry -> %exit)"), std::string::npos);
}

TEST(MAIROptimizationConfig, AddressSanitizerIgnoresKindAndAtomic) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %read = load atomic i32, ptr %p monotonic, align 4
  store i32 %read, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &Read = BB.front();
  llvm::Instruction &Write = *std::next(BB.begin());
  const MAIROptimizationConfig Config =
      MAIROptimizationConfig::forAddressSanitizer();
  MAIRGraph Graph(F, Config);
  Observation &ReadAccess = *Graph.createInstProgramPoint(BB, Read).begin();
  Observation &WriteAccess = *Graph.createInstProgramPoint(BB, Write).begin();

  EXPECT_EQ(ReadAccess.getLevel(), WriteAccess.getLevel());
  EXPECT_TRUE(ReadAccess.isMergeable());
  EXPECT_TRUE(WriteAccess.isMergeable());
  EXPECT_TRUE(ReadAccess.covers(WriteAccess));
  EXPECT_TRUE(WriteAccess.covers(ReadAccess));
  EXPECT_FALSE(ReadAccess.isMovable());
  EXPECT_TRUE(WriteAccess.isMovable());
  EXPECT_TRUE(ReadAccess.isDeletable());
  EXPECT_TRUE(WriteAccess.isDeletable());
  EXPECT_TRUE(WriteAccess.mergeWith(ReadAccess));
  EXPECT_TRUE(WriteAccess.isReadWrite());
  EXPECT_TRUE(WriteAccess.isAtomic());
  EXPECT_TRUE(WriteAccess.isMergeable());
  EXPECT_FALSE(WriteAccess.isMovable());
  EXPECT_TRUE(WriteAccess.isDeletable());
  EXPECT_EQ(WriteAccess.getLevel(), ReadAccess.getLevel());
  EXPECT_EQ(WriteAccess.getInstructions(),
            (llvm::ArrayRef<llvm::Instruction *>{&Write, &Read}));
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, GraphInitializationStoresAccessCapabilities) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
define void @f(ptr %p) {
entry:
  %normal = load i32, ptr %p, align 4
  %atomic = load atomic i32, ptr %p monotonic, align 4
  %volatile = load volatile i32, ptr %p, align 4
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 4, i1 false)
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  MAIRGraph Graph(F);
  Observation &Normal = *Graph.createInstProgramPoint(BB, *It++).begin();
  Observation &Atomic = *Graph.createInstProgramPoint(BB, *It++).begin();
  Observation &Volatile = *Graph.createInstProgramPoint(BB, *It++).begin();
  Observation &CallLike = *Graph.createInstProgramPoint(BB, *It++).begin();

  EXPECT_TRUE(Normal.isMergeable());
  EXPECT_TRUE(Normal.isMovable());
  EXPECT_TRUE(Normal.isDeletable());
  EXPECT_EQ(Normal.getLevel(), 1u);

  EXPECT_FALSE(Atomic.isMergeable());
  EXPECT_FALSE(Atomic.isMovable());
  EXPECT_FALSE(Atomic.isDeletable());
  EXPECT_FALSE(Volatile.isMergeable());
  EXPECT_TRUE(Volatile.isMovable());
  EXPECT_FALSE(Volatile.isDeletable());
  EXPECT_FALSE(CallLike.isMergeable());
  EXPECT_FALSE(CallLike.isMovable());
  EXPECT_FALSE(CallLike.isDeletable());
  EXPECT_EQ(CallLike.getLevel(), 2u);
}

TEST(MAIROptimizationConfig, ThreadSanitizerWriteOnlyCoversRead) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %read = load i32, ptr %p, align 4
  store i32 %read, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &Read = BB.front();
  llvm::Instruction &Write = *std::next(BB.begin());
  const MAIROptimizationConfig Config =
      MAIROptimizationConfig::forThreadSanitizer();
  MAIRGraph Graph(F, Config);
  Observation &ReadAccess = *Graph.createInstProgramPoint(BB, Read).begin();
  Observation &WriteAccess = *Graph.createInstProgramPoint(BB, Write).begin();

  EXPECT_GT(WriteAccess.getLevel(), ReadAccess.getLevel());
  EXPECT_TRUE(ReadAccess.isMergeable());
  EXPECT_TRUE(WriteAccess.isMergeable());
  EXPECT_TRUE(WriteAccess.covers(ReadAccess));
  EXPECT_FALSE(ReadAccess.covers(WriteAccess));
  EXPECT_TRUE(ReadAccess.isDeletable());
  EXPECT_TRUE(WriteAccess.isDeletable());
  EXPECT_FALSE(ReadAccess.mergeWith(WriteAccess));
}

TEST(MAIROptimizationConfig, ThreadSanitizerPinsAtomicAccesses) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %first = load atomic i32, ptr %p monotonic, align 4
  %second = load atomic i32, ptr %p monotonic, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &First = BB.front();
  llvm::Instruction &Second = *std::next(BB.begin());
  const MAIROptimizationConfig Config =
      MAIROptimizationConfig::forThreadSanitizer();
  MAIRGraph Graph(F, Config);
  ProgramPoint &Available = Graph.createInstProgramPoint(BB, First);
  ProgramPoint &Required = Graph.createInstProgramPoint(BB, Second);
  Graph.addEdge(Available, Required, PPEdgeKind::Dom);

  EXPECT_FALSE(Available.begin()->mergeWith(*Required.begin()));
  EXPECT_FALSE(Available.begin()->covers(*Required.begin()));
  EXPECT_FALSE(Available.begin()->isMergeable());
  EXPECT_FALSE(Available.begin()->isDeletable());
  EXPECT_FALSE(Required.begin()->isMergeable());
  EXPECT_FALSE(Required.begin()->isDeletable());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Required.size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, RedundantEliminationUsesSanitizerCoverage) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @write_then_read(ptr %p) {
entry:
  store i32 0, ptr %p, align 4
  %read = load i32, ptr %p, align 4
  ret void
}
define void @read_then_write(ptr %p) {
entry:
  %read = load i32, ptr %p, align 4
  store i32 %read, ptr %p, align 4
  ret void
}
define void @asan_atomic(ptr %p) {
entry:
  %first = load atomic i32, ptr %p monotonic, align 4
  %second = load atomic i32, ptr %p monotonic, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  auto Run = [&](llvm::StringRef Name, const MAIROptimizationConfig &Config) {
    llvm::Function &F = *M->getFunction(Name);
    llvm::BasicBlock &BB = F.getEntryBlock();
    llvm::Instruction &First = BB.front();
    llvm::Instruction &Second = *std::next(BB.begin());
    MAIRGraph Graph(F, Config);
    ProgramPoint &Available = Graph.createInstProgramPoint(BB, First);
    ProgramPoint &Required = Graph.createInstProgramPoint(BB, Second);
    Graph.addEdge(Available, Required, PPEdgeKind::Dom);

    llvm::PassInstrumentationCallbacks PIC;
    MAIRGraphAnalysisManager AM;
    registerMAIRGraphAnalyses(AM, &PIC);
    RedundantObservationEliminationPass Pass;
    Pass.run(Graph, AM);
    EXPECT_TRUE(Graph.verify());
    return Required.empty();
  };

  const MAIROptimizationConfig TSan =
      MAIROptimizationConfig::forThreadSanitizer();
  EXPECT_TRUE(Run("write_then_read", TSan));
  EXPECT_FALSE(Run("read_then_write", TSan));
  EXPECT_TRUE(
      Run("asan_atomic", MAIROptimizationConfig::forAddressSanitizer()));
}

TEST(MAIROptimizationConfig, AddressSanitizerRejectsPostDomCoverage) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %first = load atomic i32, ptr %p monotonic, align 4
  %second = load atomic i32, ptr %p monotonic, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction &First = BB.front();
  llvm::Instruction &Second = *std::next(BB.begin());
  MAIRGraph Graph(F, MAIROptimizationConfig::forAddressSanitizer());
  ProgramPoint &Earlier = Graph.createInstProgramPoint(BB, First);
  ProgramPoint &Later = Graph.createInstProgramPoint(BB, Second);
  Graph.addEdge(Later, Earlier, PPEdgeKind::PostDom);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Earlier.size(), 1u);
  EXPECT_EQ(Later.size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, CallAccessesStayIndependent) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
define void @f(ptr %p) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 4, i1 false)
  call void @llvm.memset.p0.i64(ptr %p, i8 1, i64 4, i1 false)
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto CheckConfig = [&](const MAIROptimizationConfig &Config) {
    MAIRGraph Graph(F, Config);
    Observation &First = *Graph.createInstProgramPoint(BB, BB.front()).begin();
    Observation &Second =
        *Graph.createInstProgramPoint(BB, *std::next(BB.begin())).begin();
    EXPECT_TRUE(First.isCallLike());
    EXPECT_TRUE(Second.isCallLike());
    EXPECT_FALSE(First.isMergeable());
    EXPECT_FALSE(Second.isMergeable());
    EXPECT_FALSE(First.isDeletable());
    EXPECT_FALSE(Second.isDeletable());
    expectMergeRejectedWithoutMutation(First, Second);
  };
  CheckConfig(MAIROptimizationConfig::forAddressSanitizer());
  CheckConfig(MAIROptimizationConfig::forThreadSanitizer());
}

TEST(MAIROptimizationConfig, ConstantMaskedRegionsUseActiveLanes) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, <4 x i1>, <4 x i32>)
define void @f(ptr %p) {
entry:
  %wide = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> <i1 true, i1 false, i1 true, i1 false>,
      <4 x i32> zeroinitializer)
  %narrow = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> <i1 true, i1 false, i1 false, i1 false>,
      <4 x i32> zeroinitializer)
  %extra = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  const MAIROptimizationConfig Config =
      MAIROptimizationConfig::forAddressSanitizer();
  MAIRGraph Graph(F, Config);
  Observation &Wide = *Graph.createInstProgramPoint(BB, BB.front()).begin();
  Observation &Narrow =
      *Graph.createInstProgramPoint(BB, *std::next(BB.begin())).begin();

  EXPECT_TRUE(Wide.isMasked());
  EXPECT_FALSE(Wide.isCallLike());
  ASSERT_NE(Wide.getMaskedRegion(), nullptr);
  EXPECT_TRUE(Wide.covers(Narrow));
  EXPECT_FALSE(Narrow.covers(Wide))
      << "narrow=" << Narrow.toString() << " wide=" << Wide.toString();
  EXPECT_TRUE(Narrow.mergeWith(Wide));
  EXPECT_TRUE(Narrow.covers(Wide));
  llvm::Instruction *First = &BB.front();
  llvm::Instruction *Second = &*std::next(BB.begin());
  llvm::Instruction *Third = &*std::next(BB.begin(), 2);
  for (unsigned Lanes : {4u, 128u}) {
    for (const auto &Bits : {std::pair{3u, 1u},
                             {1u, 3u},
                             {3u, 3u},
                             {1u, 2u},
                             {0u, 0u},
                             {0u, 1u},
                             {1u, 0u}}) {
      SCOPED_TRACE(::testing::Message() << Lanes << " lanes: " << Bits.first
                                        << " + " << Bits.second);
      auto MakeRegion = [&](unsigned Active) {
        llvm::SmallBitVector Mask(Lanes);
        if (Active & 1)
          Mask.set(0);
        if (Active & 2)
          Mask.set(Lanes - 1);
        return MaskedRegion(AbstractAccess(F.getArg(0),
                                           AbstractValue::constant(0),
                                           AbstractValue::constant(4 * Lanes)),
                            std::move(Mask), 4);
      };
      MaskedRegion LeftRegion = MakeRegion(Bits.first);
      MaskedRegion RightRegion = MakeRegion(Bits.second);
      const bool LeftContains = (Bits.second & Bits.first) == Bits.second;
      const bool RightContains = (Bits.first & Bits.second) == Bits.first;
      EXPECT_EQ(LeftRegion.contains(RightRegion), LeftContains);
      EXPECT_EQ(RightRegion.contains(LeftRegion), RightContains);
      Observation Left({First, Second, First}, MemoryRegion(LeftRegion),
                       AccessKind::Read);
      Observation Right({Third, Second}, MemoryRegion(RightRegion),
                        AccessKind::Read);
      ASSERT_TRUE(Left.mergeWith(Right));
      llvm::SmallVector<llvm::Instruction *, 5> Expected{First, Second, First,
                                                         Third, Second};
      if (RightContains && !LeftContains)
        Expected = {Third, Second, First, Second, First};
      EXPECT_EQ(Left.getInstructions(), llvm::ArrayRef(Expected));
      EXPECT_EQ(Left.getInstruction(), Expected.front());
      EXPECT_EQ(Left.getRegion(),
                MemoryRegion(MakeRegion(Bits.first | Bits.second)));
    }
  }
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, DynamicMasksRequireTheSameSSAValue) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, <4 x i1>, <4 x i32>)
define void @f(ptr %p, <4 x i1> %a, <4 x i1> %b) {
entry:
  %first = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> %a, <4 x i32> zeroinitializer)
  %same = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> %a, <4 x i32> zeroinitializer)
  %different = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> %b, <4 x i32> zeroinitializer)
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  const MAIROptimizationConfig Config =
      MAIROptimizationConfig::forAddressSanitizer();
  MAIRGraph Graph(F, Config);
  Observation &First = *Graph.createInstProgramPoint(BB, *It++).begin();
  Observation &Same = *Graph.createInstProgramPoint(BB, *It++).begin();
  Observation &Different = *Graph.createInstProgramPoint(BB, *It).begin();

  EXPECT_TRUE(First.covers(Same));
  EXPECT_FALSE(First.covers(Different))
      << "first=" << First.toString() << " different=" << Different.toString();
  EXPECT_TRUE(First.mergeWith(Same));
  EXPECT_EQ(First.getInstructions(),
            (llvm::ArrayRef<llvm::Instruction *>{&BB.front(),
                                                 &*std::next(BB.begin())}));
  expectMergeRejectedWithoutMutation(First, Different);
  expectMergeRejectedWithoutMutation(Different, First);
  const MaskedRegion &Bounds = *First.getMaskedRegion();
  Observation Constant(
      {&BB.front()},
      MemoryRegion(
          MaskedRegion(Bounds.getBounds(), llvm::SmallBitVector(4, true), 4)),
      AccessKind::Read);
  expectMergeRejectedWithoutMutation(First, Constant);
  expectMergeRejectedWithoutMutation(Constant, First);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, LocationMergeJoinsEquivalentNonConcreteLocations) {
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
  llvm::Argument &P = *F.arg_begin();
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);

  auto MakeAccess = [&](uint64_t Offset) {
    return Observation(
        {&Load},
        MemoryRegion(AbstractAccess(&P, AbstractValue::constant(Offset),
                                    AbstractValue::unknown())),
        AccessKind::Read);
  };

  PP.addObservation(MakeAccess(8));
  PP.addObservation(MakeAccess(0));
  PP.addObservation(MakeAccess(8));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(PP.size(), 2u);

  auto It = PP.begin();
  ASSERT_NE(It->getContiguousRegion(), nullptr);
  EXPECT_EQ(It->getContiguousRegion()->getOffset(), AbstractValue::constant(0));
  ++It;
  ASSERT_NE(It->getContiguousRegion(), nullptr);
  EXPECT_EQ(It->getContiguousRegion()->getOffset(), AbstractValue::constant(8));
  EXPECT_EQ(It->getInstructions().size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, MergeRejectsWrappingSymbolicOffsets) {
  llvm::LLVMContext Context;
  auto Module = parseModule(R"IR(
target datalayout = "e-p:64:64"
define void @wrapping(ptr %base, i8 %index) {
entry:
  %first = getelementptr i8, ptr %base, i8 %index
  %next = add i8 %index, 1
  %second = getelementptr i8, ptr %base, i8 %next
  %first.value = load i8, ptr %first
  %second.value = load i8, ptr %second
  ret void
}
)IR",
                            Context);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("wrapping");
  auto Instructions = Function.getEntryBlock().begin();
  std::advance(Instructions, 3);
  Observation First =
      Observation::fromInstruction(*Instructions++, Module->getDataLayout());
  Observation Second =
      Observation::fromInstruction(*Instructions, Module->getDataLayout());
  expectMergeRejectedWithoutMutation(First, Second);
  llvm::PassInstrumentationCallbacks Callbacks;
  llvm::FunctionAnalysisManager Analyses;
  registerLoopAnalyses(Analyses, Callbacks);
  auto &Evolution = Analyses.getResult<llvm::ScalarEvolutionAnalysis>(Function);
  Instructions = Function.getEntryBlock().begin();
  std::advance(Instructions, 3);
  Observation FirstWithSE =
      Observation::fromInstruction(*Instructions++, Module->getDataLayout());
  Observation SecondWithSE =
      Observation::fromInstruction(*Instructions, Module->getDataLayout());
  expectMergeRejectedWithoutMutation(FirstWithSE, SecondWithSE, &Evolution);
}

TEST(MAIROptimizationConfig, MergeRejectsUnrepresentablePointerDistance) {
  llvm::LLVMContext Context;
  auto Module = parseModule(R"IR(
target datalayout = "e-p:128:128:128:128"
define void @wide(ptr %base) {
entry:
  %first = getelementptr i8, ptr %base, i128 -18446744073709551616
  %second = getelementptr i8, ptr %base, i128 -1
  %first.value = load i8, ptr %first
  %second.value = load i8, ptr %second
  ret void
}
)IR",
                            Context);
  ASSERT_TRUE(Module);
  llvm::Function &Function = *Module->getFunction("wide");
  auto Instructions = Function.getEntryBlock().begin();
  auto First = AbstractAccess::fromPointerAndSize(
      *Instructions++, AbstractValue::constant(1), Module->getDataLayout());
  auto Second = AbstractAccess::fromPointerAndSize(
      *Instructions, AbstractValue::constant(1), Module->getDataLayout());
  EXPECT_FALSE(AbstractAccess::merge(First, Second, &Module->getDataLayout()));
  EXPECT_FALSE(AbstractAccess::merge(Second, First, &Module->getDataLayout()));
  llvm::PassInstrumentationCallbacks Callbacks;
  llvm::FunctionAnalysisManager Analyses;
  registerLoopAnalyses(Analyses, Callbacks);
  auto &Evolution = Analyses.getResult<llvm::ScalarEvolutionAnalysis>(Function);
  EXPECT_FALSE(AbstractAccess::merge(First, Second, &Module->getDataLayout(),
                                     &Evolution));
}

TEST(MAIROptimizationConfig, MergeWithJoinsAdjacentSymbolicLocations) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i64 %x) {
entry:
  %px = getelementptr i8, ptr %p, i64 %x
  store i32 0, ptr %px, align 4
  %px4 = getelementptr i8, ptr %px, i64 4
  store i32 1, ptr %px4, align 4
  %px12 = getelementptr i8, ptr %px, i64 12
  store i32 2, ptr %px12, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::Argument &P = *F.arg_begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  ++It;
  llvm::Instruction &LowerStore = *It++;
  ++It;
  llvm::Instruction &UpperStore = *It++;
  ++It;
  llvm::Instruction &GappedStore = *It;

  Observation Lower =
      Observation::fromInstruction(LowerStore, M->getDataLayout());
  Observation Upper =
      Observation::fromInstruction(UpperStore, M->getDataLayout());

  ASSERT_EQ(Lower.getBase(), &P);
  ASSERT_EQ(Upper.getBase(), &P);
  ASSERT_TRUE(Lower.mergeWith(Upper));
  const AbstractAccess *Merged = Lower.getContiguousRegion();
  ASSERT_NE(Merged, nullptr);
  // gep i8 %p, %x is the byte offset %x, not the GEP itself.
  EXPECT_EQ(Merged->getOffset(),
            AbstractValue::symbol(*std::next(F.arg_begin())));
  EXPECT_EQ(Merged->getSize(), AbstractValue::constant(8));
  EXPECT_TRUE(Lower.isWrite());

  Observation ReversedUpper =
      Observation::fromInstruction(UpperStore, M->getDataLayout());
  Observation ReversedLower =
      Observation::fromInstruction(LowerStore, M->getDataLayout());
  ASSERT_TRUE(ReversedUpper.mergeWith(ReversedLower));
  Merged = ReversedUpper.getContiguousRegion();
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->getOffset(),
            AbstractValue::symbol(*std::next(F.arg_begin())));
  EXPECT_EQ(Merged->getSize(), AbstractValue::constant(8));

  Observation GappedLower =
      Observation::fromInstruction(LowerStore, M->getDataLayout());
  Observation GappedUpper =
      Observation::fromInstruction(GappedStore, M->getDataLayout());
  expectMergeRejectedWithoutMutation(GappedLower, GappedUpper);
}

TEST(MAIROptimizationConfig,
     LocationMergeJoinsAdjacentSymbolicLocationsWithoutExternalKey) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i64 %x) {
entry:
  %px = getelementptr i8, ptr %p, i64 %x
  store i32 0, ptr %px, align 4
  %px4 = getelementptr i8, ptr %px, i64 4
  store i32 1, ptr %px4, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  ++It;
  llvm::Instruction &LowerStore = *It++;
  ++It;
  llvm::Instruction &UpperStore = *It;
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);
  PP.addObservation(
      Observation::fromInstruction(LowerStore, M->getDataLayout()));
  PP.addObservation(
      Observation::fromInstruction(UpperStore, M->getDataLayout()));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(PP.size(), 1u);

  const AbstractAccess *Merged = PP.begin()->getContiguousRegion();
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->getBase(), F.arg_begin());
  EXPECT_EQ(Merged->getSize(), AbstractValue::constant(8));
  EXPECT_TRUE(PP.begin()->isWrite());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, LocationMergeKeepsConcreteRangesInLinearSweep) {
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
  llvm::Argument &P = *F.arg_begin();
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);

  PP.addObservation(
      Observation({&Load},
                  MemoryRegion(AbstractAccess(&P, AbstractValue::constant(0),
                                              AbstractValue::constant(8))),
                  AccessKind::Read));
  PP.addObservation(
      Observation({&Load},
                  MemoryRegion(AbstractAccess(&P, AbstractValue::constant(4),
                                              AbstractValue::unknown())),
                  AccessKind::Read));
  PP.addObservation(
      Observation({&Load},
                  MemoryRegion(AbstractAccess(&P, AbstractValue::constant(8),
                                              AbstractValue::constant(4))),
                  AccessKind::Read));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(PP.size(), 2u);

  auto It = PP.begin();
  ASSERT_NE(It->getContiguousRegion(), nullptr);
  EXPECT_EQ(It->getContiguousRegion()->getOffset(), AbstractValue::constant(0));
  EXPECT_EQ(It->getContiguousRegion()->getSize(), AbstractValue::constant(12));
  ++It;
  ASSERT_NE(It->getContiguousRegion(), nullptr);
  EXPECT_EQ(It->getContiguousRegion()->getOffset(), AbstractValue::constant(4));
  EXPECT_TRUE(It->getContiguousRegion()->getSize().isUnknown());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig,
     LocationMergeDoesNotLetAnotherLevelBlockConcreteMerge) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %left = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  store i32 0, ptr %p4, align 4
  %right = load i32, ptr %p4, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  llvm::Instruction &Left = *It++;
  ++It;
  llvm::Instruction &Barrier = *It++;
  llvm::Instruction &Right = *It;
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);
  PP.addObservation(Observation::fromInstruction(Left, M->getDataLayout()));
  PP.addObservation(Observation::fromInstruction(Barrier, M->getDataLayout()));
  PP.addObservation(Observation::fromInstruction(Right, M->getDataLayout()));

  ASSERT_EQ(PP.begin()->getLevel(), std::prev(PP.end())->getLevel());
  ASSERT_NE(PP.begin()->getLevel(), std::next(PP.begin())->getLevel());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(PP.size(), 2u);

  const Observation &Merged = *PP.begin();
  ASSERT_NE(Merged.getContiguousRegion(), nullptr);
  EXPECT_EQ(Merged.getLevel(), 1u);
  EXPECT_EQ(Merged.getContiguousRegion()->getOffset(),
            AbstractValue::constant(0));
  EXPECT_EQ(Merged.getContiguousRegion()->getSize(),
            AbstractValue::constant(8));
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig,
     LocationMergeDoesNotLetUnmergeableAccessBlockConcreteMerge) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %left = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  %barrier = load atomic i32, ptr %p4 monotonic, align 4
  %right = load i32, ptr %p4, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  llvm::Instruction &Left = *It++;
  ++It;
  llvm::Instruction &Barrier = *It++;
  llvm::Instruction &Right = *It;
  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createBBProgramPoint(BB, true);
  PP.addObservation(Observation::fromInstruction(Left, M->getDataLayout()));
  PP.addObservation(Observation::fromInstruction(Barrier, M->getDataLayout()));
  PP.addObservation(Observation::fromInstruction(Right, M->getDataLayout()));

  ASSERT_TRUE(PP.begin()->isMergeable());
  ASSERT_FALSE(std::next(PP.begin())->isMergeable());
  ASSERT_TRUE(std::prev(PP.end())->isMergeable());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(PP.size(), 2u);

  auto Merged = llvm::find_if(
      PP, [](const Observation &Obs) { return Obs.isMergeable(); });
  ASSERT_NE(Merged, PP.end());
  ASSERT_NE(Merged->getContiguousRegion(), nullptr);
  EXPECT_EQ(Merged->getContiguousRegion()->getOffset(),
            AbstractValue::constant(0));
  EXPECT_EQ(Merged->getContiguousRegion()->getSize(),
            AbstractValue::constant(8));
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIROptimizationConfig, LocationMergeUnionsComparableMasksOnly) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, <4 x i1>, <4 x i32>)
define void @f(ptr %p) {
entry:
  %low = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> <i1 true, i1 false, i1 false, i1 false>,
      <4 x i32> zeroinitializer)
  %high = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> <i1 false, i1 false, i1 true, i1 false>,
      <4 x i32> zeroinitializer)
  %opaque = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> <i1 poison, i1 false, i1 false, i1 false>,
      <4 x i32> zeroinitializer)
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  llvm::Instruction &Low = *It++;
  llvm::Instruction &High = *It++;
  llvm::Instruction &Opaque = *It;
  const MAIROptimizationConfig Config =
      MAIROptimizationConfig::forAddressSanitizer();
  MAIRGraph Graph(F, Config);
  ProgramPoint &PP = Graph.createInstProgramPoint(BB, Low);
  PP.addObservation(Observation::fromInstruction(High, M->getDataLayout()));
  PP.addObservation(Observation::fromInstruction(Opaque, M->getDataLayout()));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(PP.size(), 2u);
  const MaskedRegion *Merged = PP.begin()->getMaskedRegion();
  ASSERT_NE(Merged, nullptr);
  ASSERT_TRUE(Merged->isConstantMask());
  EXPECT_TRUE(Merged->getConstantMask().test(0));
  EXPECT_TRUE(Merged->getConstantMask().test(2));
  const MaskedRegion *Unmerged = std::next(PP.begin())->getMaskedRegion();
  ASSERT_NE(Unmerged, nullptr);
  EXPECT_FALSE(Unmerged->isComparable());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, GuardedAnchoredDumpDistinguishesSiteKinds) {
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
  ProgramPointGuard Guard = AnyProvenanceExecutedGuard({&Load});
  Graph.createProgramPoint(Entry.getSite(), Guard);
  Graph.createProgramPoint(
      ProgramPointSite::anchored(Load, ProgramPoint::SiteKind::MemoryInst),
      Guard);

  ASSERT_TRUE(Graph.verify());
  std::string Text = Graph.toString();
  EXPECT_NE(Text.find("site=bb.entry(%entry)"), std::string::npos);
  EXPECT_NE(Text.find("site=memory(%entry, %v)"), std::string::npos);

  std::string DOTText;
  llvm::raw_string_ostream DOTOS(DOTText);
  dumpDOT(Graph, DOTOS);
  DOTOS.flush();
  EXPECT_NE(DOTText.find("bb.entry(%entry)"), std::string::npos);
  EXPECT_NE(DOTText.find("memory(%entry, %v)"), std::string::npos);
}

TEST(Observation, StableDebugLocComesFromTheOriginalAccess) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %a = load i32, ptr %p, align 4
  %b = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &BB = F.getEntryBlock();
  auto It = BB.begin();
  llvm::Instruction &First = *It++;
  llvm::Instruction &Second = *It++;

  llvm::DIBuilder DIB(*M);
  llvm::DIFile *File = DIB.createFile("simple_stack.c", "/");
  llvm::DICompileUnit *CU =
      DIB.createCompileUnit(llvm::DISourceLanguageName(llvm::dwarf::DW_LANG_C),
                            File, "clang", false, "", 0);
  llvm::DISubroutineType *Type =
      DIB.createSubroutineType(DIB.getOrCreateTypeArray({}));
  llvm::DISubprogram *SP =
      DIB.createFunction(CU, "f", "f", File, 1, Type, 1, llvm::DINode::FlagZero,
                         llvm::DISubprogram::SPFlagDefinition);
  F.setSubprogram(SP);
  llvm::DebugLoc AtGlobal = llvm::DILocation::get(Ctx, 15, 20, SP);
  llvm::DebugLoc AtAnchor = llvm::DILocation::get(Ctx, 15, 16, SP);
  DIB.finalize();

  MemoryRegion Region(AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                                     AbstractValue::constant(4)));
  Observation None({&First, &Second}, Region, AccessKind::Read);
  EXPECT_EQ(None.getStableDebugLoc().get(), nullptr);

  Second.setDebugLoc(AtAnchor);
  Observation Later({&First, &Second}, Region, AccessKind::Read);
  EXPECT_EQ(Later.getStableDebugLoc().get(), AtAnchor.get());

  First.setDebugLoc(AtGlobal);
  Observation Earliest({&First, &Second}, Region, AccessKind::Read);
  EXPECT_EQ(Earliest.getStableDebugLoc().get(), AtGlobal.get());
  Observation Equal({&Second, &First}, Region, AccessKind::Read);
  ASSERT_TRUE(Earliest.mergeWith(Equal));
  EXPECT_EQ(Earliest.getInstruction(), &First);
  EXPECT_EQ(Earliest.getStableDebugLoc().get(), AtGlobal.get());
  Observation Covering({&Second, &First},
                       AbstractAccess(F.getArg(0), AbstractValue::constant(0),
                                      AbstractValue::constant(8)),
                       AccessKind::Read);
  ASSERT_TRUE(Earliest.mergeWith(Covering));
  EXPECT_EQ(Earliest.getInstruction(), &Second);
  EXPECT_EQ(Earliest.getStableDebugLoc().get(), AtAnchor.get());
}

TEST(MAIRStatLog, CountsPassInputsOnlyWhenStatisticsConsumeThem) {
  // Counting walks the whole graph, so a pass must not pay for it when
  // neither LLVM statistics nor -mair-stats will use the result.
  struct InputCounter {
    uint64_t Total = 0;
    void operator+=(uint64_t N) { Total += N; }
  } Counter;
  unsigned Calls = 0;
  MAIRGraph Graph;
  ASSERT_FALSE(mairStatsEnabled());
  {
    MAIRStatScope Scope("test", /*Ordinal=*/0, Graph, Counter, [&] {
      ++Calls;
      return 7u;
    });
  }
  const bool StatisticsCompiledIn = LLVM_ENABLE_STATS;
  EXPECT_EQ(Calls, StatisticsCompiledIn ? 1u : 0u);
  EXPECT_EQ(Counter.Total, StatisticsCompiledIn ? 7u : 0u);
}
