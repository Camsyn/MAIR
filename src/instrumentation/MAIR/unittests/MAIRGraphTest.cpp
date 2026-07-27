#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRBarrierAnalysis.h"
#include "MAIR/MAIRDump.h"
#include "MAIR/MAOpt.h"
#include "MAIR/Transforms/CommonMAMotionPass.h"
#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <type_traits>
#include <utility>

using namespace mair;

namespace {
static_assert(
    std::is_same_v<decltype(std::declval<MemoryAccess &>().getMemoryObject()),
                   MemoryObject &>);
static_assert(std::is_same_v<
              decltype(std::declval<const MemoryAccess &>().getMemoryObject()),
              const MemoryObject &>);
static_assert(!std::is_default_constructible_v<ObservationSite>);
static_assert(!std::is_default_constructible_v<Observation>);

std::unique_ptr<llvm::Module> parseModule(llvm::StringRef IR,
                                          llvm::LLVMContext &Ctx) {
  llvm::SMDiagnostic Err;
  auto M = llvm::parseAssemblyString(IR, Err, Ctx);
  return M;
}

template <class BarrierTrait = DefaultBarrierTrait>
MAIRGraph createGraphWithExplicitAnalyses(Function &F) {
  llvm::DominatorTree DT(F);
  llvm::PostDominatorTree PDT(F);
  llvm::LoopInfo LI(DT);
  return MAIRGraph::build<BarrierTrait>(F, std::move(DT), std::move(PDT),
                                        std::move(LI));
}

bool compareTestAccesses(const MemoryAccess &LHS, const MemoryAccess &RHS) {
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

TEST(MAOpt, BuildsOptimizesAndExposesObservationAccesses) {
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

  size_t NumMappedAccesses = 0;
  for (const auto &Entry : Opt.getObservationMAs()) {
    ASSERT_NE(Entry.first, nullptr);
    EXPECT_EQ(Entry.second.size(), Opt.getObservationMAs(*Entry.first).size());
    for (const MemoryAccess *Access : Entry.second) {
      ASSERT_NE(Access, nullptr);
      EXPECT_EQ(Access->getInstructions().size(), 1u);
      ++NumMappedAccesses;
    }
  }
  EXPECT_EQ(NumMappedAccesses, 2u);

  MAOpt<> O1(F, FAM, &PIC, /*KeepFullDebug=*/false);
  O1.build();
  ASSERT_FALSE(O1.optimize());
  EXPECT_TRUE(O1.getGraph().verify());
}

TEST(MAOpt, RetainsAccessesForTargetInstructions) {
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
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::Instruction *Target = &Entry.front();
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

  llvm::SmallPtrSet<const llvm::Instruction *, 1> Targets;
  Targets.insert(Target);
  Opt.retainAccessesForInstructions(Targets);

  size_t NumAccesses = 0;
  for (const Observation &Obs : Graph)
    NumAccesses += Obs.size();
  EXPECT_EQ(NumAccesses, 1u);
  EXPECT_EQ(llvm::range_size(PObject->users()), 1u);
  EXPECT_EQ(llvm::range_size(QObject->users()), 0u);
  EXPECT_TRUE(Graph.verify());
}

bool hasSuccessor(const MAIRGraph &Graph, const Observation &From,
                  const Observation &To) {
  (void)Graph;
  for (const ObservationEdge &Edge : From.children())
    if (&Edge.getNode() == &To)
      return true;
  return false;
}

bool hasSuccessor(const MAIRGraph &Graph, const Observation &From,
                  const Observation &To, ObsEdgeKind Kind) {
  return Graph.hasEdge(From, To, Kind);
}

const Observation *findBlockEntry(const MAIRGraph &Graph,
                                  llvm::StringRef Name) {
  for (const Observation &Obs : Graph)
    if (Obs.isBasicBlockEntrySite() && Obs.getBasicBlock() &&
        Obs.getBasicBlock()->getName() == Name)
      return &Obs;
  return nullptr;
}

const Observation *findBlockExit(const MAIRGraph &Graph, llvm::StringRef Name) {
  for (const Observation &Obs : Graph)
    if (Obs.isBasicBlockExitSite() && Obs.getBasicBlock() &&
        Obs.getBasicBlock()->getName() == Name)
      return &Obs;
  return nullptr;
}

const Observation *findMemoryObservation(const MAIRGraph &Graph,
                                         llvm::StringRef BlockName) {
  for (const Observation &Obs : Graph)
    if (Obs.isMemoryInstSite() && Obs.getBasicBlock() &&
        Obs.getBasicBlock()->getName() == BlockName)
      return &Obs;
  return nullptr;
}

Observation *findMutableMemoryObservation(MAIRGraph &Graph,
                                          llvm::StringRef BlockName) {
  for (Observation &Obs : Graph)
    if (Obs.isMemoryInstSite() && Obs.getBasicBlock() &&
        Obs.getBasicBlock()->getName() == BlockName)
      return &Obs;
  return nullptr;
}

void expectSimplifyGraphPreservesMutualPair(MAIRGraph &Graph,
                                            Observation &Earlier,
                                            Observation &Later) {
  Graph.addEdge(Earlier, Later, ObsEdgeKind::Dom);
  Graph.addEdge(Later, Earlier, ObsEdgeKind::PostDom);
  const size_t InitialSize = Graph.size();
  const std::string EarlierName = Earlier.getName().str();
  const std::string LaterName = Later.getName().str();
  ASSERT_TRUE(Graph.verify());

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), InitialSize);
  EXPECT_NE(Graph.find(EarlierName), nullptr);
  EXPECT_NE(Graph.find(LaterName), nullptr);
  EXPECT_TRUE(Graph.verify());
}

void expectSimplifyGraphContractsMutualPair(MAIRGraph &Graph,
                                            Observation &Earlier,
                                            Observation &Later) {
  Graph.addEdge(Earlier, Later, ObsEdgeKind::Dom);
  Graph.addEdge(Later, Earlier, ObsEdgeKind::PostDom);
  const size_t InitialSize = Graph.size();
  const std::string EarlierName = Earlier.getName().str();
  const std::string LaterName = Later.getName().str();
  ASSERT_TRUE(Graph.verify());

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), InitialSize - 1);
  EXPECT_EQ(Graph.find(EarlierName), nullptr);
  EXPECT_NE(Graph.find(LaterName), nullptr);
  EXPECT_TRUE(Graph.verify());
}

void moveMemoryObservationAccessesTo(MAIRGraph &Graph,
                                     llvm::StringRef BlockName,
                                     Observation &Target) {
  Observation *Source = nullptr;
  for (Observation &Obs : Graph)
    if (Obs.isMemoryInstSite() && Obs.getBasicBlock() &&
        Obs.getBasicBlock()->getName() == BlockName) {
      Source = &Obs;
      break;
    }
  ASSERT_NE(Source, nullptr);
  while (!Source->empty()) {
    MemoryAccess &Access = *Source->begin();
    Target.addAccess(Access);
    Source->erase(Access);
  }
}

void registerLoopTestAnalyses(llvm::FunctionAnalysisManager &FAM,
                              llvm::PassInstrumentationCallbacks &PIC) {
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  FAM.registerPass([] { return llvm::TargetLibraryAnalysis(); });
  FAM.registerPass([] { return llvm::TargetIRAnalysis(); });
  FAM.registerPass([] { return llvm::AssumptionAnalysis(); });
  FAM.registerPass([] { return llvm::ScalarEvolutionAnalysis(); });
}

struct CallBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::CallBase>(I) && !llvm::isa<llvm::AnyMemIntrinsic>(I);
  }

  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return isForwardBarrier(I);
  }
};

struct ForwardCallBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::CallBase>(I);
  }

  static bool isBackwardBarrier(const llvm::Instruction &) { return false; }
};

struct BackwardCallBarrierTrait {
  static bool isForwardBarrier(const llvm::Instruction &) { return false; }

  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return llvm::isa<llvm::CallBase>(I);
  }
};

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
  auto &Entry = Graph.createBBObservation(BB, true);
  auto &Obs = Graph.createBBObservation(BB, true);
  Obs.addAccess(MemoryAccess::fromInstruction(Load, M->getDataLayout()));
  Observation Other("o1", Obs.getSite(), Obs.getGuard());
  Other.addAccess(MemoryAccess::fromInstruction(Load, M->getDataLayout()));
  Obs.mergeAccessesFrom(Other);
  Graph.addEdge(Entry, Obs, ObsEdgeKind::Dom);

  EXPECT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.size(), 2u);
  EXPECT_TRUE(Entry.isBasicBlockEntrySite());
  EXPECT_TRUE(Obs.isBasicBlockEntrySite());
  ASSERT_EQ(Entry.children().size(), 1u);
  EXPECT_EQ(&Entry.children()[0].getNode(), &Obs);
  EXPECT_EQ(&Obs.parents()[0].getNode(), &Entry);
  ASSERT_EQ(Obs.accesses().size(), 2u);
  for (const MemoryAccess &Access : Obs)
    EXPECT_EQ(Access.getParent(), &Obs);

  unsigned ObsCount = 0;
  unsigned AccessCount = 0;
  for (const Observation &Node : Graph) {
    ++ObsCount;
    for (const MemoryAccess &Access : Node) {
      (void)Access;
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
  EXPECT_NE(Dump.find("bs0: preds=[]"), std::string::npos);
  EXPECT_NE(Dump.find("bs1: preds=[bs0(dom)]"), std::string::npos);
  EXPECT_NE(Dump.find("read ptr %p, 4"), std::string::npos);
  EXPECT_NE(Dump.find("succs=[bs1(dom)]"), std::string::npos);
  EXPECT_EQ(Dump.find("site="), std::string::npos);
  EXPECT_EQ(Dump.find("guard="), std::string::npos);
}

TEST(MAIRSchema, EveryObservationDerivesItsBlockFromItsAnchor) {
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
  Observation &EntryObs = Graph.createBBObservation(Entry, true);
  Observation &MemoryObs = Graph.createInstObservation(Entry, Load);
  Observation &ExitObs = Graph.createBBObservation(Entry, false);
  Observation &EdgeObs = Graph.createEdgeObservation(Entry, Exit);

  EXPECT_EQ(&EntryObs.getAnchor(), &Load);
  EXPECT_EQ(&MemoryObs.getAnchor(), &Load);
  EXPECT_EQ(&ExitObs.getAnchor(), &Terminator);
  EXPECT_EQ(&EdgeObs.getAnchor(), &Terminator);
  for (const Observation *Obs : {&EntryObs, &MemoryObs, &ExitObs, &EdgeObs})
    EXPECT_EQ(Obs->getBasicBlock(), Obs->getAnchor().getParent());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraph, FindsTypedObservationAncestors) {
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
  Observation &Entry = Graph.createBBObservation(BB, true);
  Observation &Memory = Graph.createInstObservation(BB, Load);
  Observation &Exit = Graph.createBBObservation(BB, false);
  Graph.addEdge(Entry, Memory, ObsEdgeKind::Dom);
  Graph.addEdge(Memory, Exit, ObsEdgeKind::Dom);
  Graph.addEdge(Exit, Memory, ObsEdgeKind::PostDom);
  Graph.addEdge(Memory, Entry, ObsEdgeKind::PostDom);

  EXPECT_TRUE(Graph.isAncestor(Entry, Exit, ObsEdgeKind::Dom));
  EXPECT_TRUE(Graph.isAncestor(Exit, Entry, ObsEdgeKind::PostDom));
  EXPECT_FALSE(Graph.isAncestor(Entry, Exit, ObsEdgeKind::PostDom));
  EXPECT_FALSE(Graph.isAncestor(Exit, Entry, ObsEdgeKind::Dom));
  EXPECT_FALSE(Graph.isAncestor(Memory, Memory, ObsEdgeKind::Dom));
  EXPECT_TRUE(Graph.verify());
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

TEST(MAIRGraph, GraphTraitsExposeObservationGraph) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  ASSERT_TRUE(Graph.verify());

  const Observation *Entry = Graph.find("bs0");
  const Observation *Load = Graph.find("ma1");
  const Observation *Store = Graph.find("ma2");
  const Observation *Exit = Graph.find("be3");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  ASSERT_NE(Exit, nullptr);

  llvm::SmallVector<const Observation *, 4> Nodes;
  for (const Observation *Obs : llvm::nodes<const MAIRGraph *>(&Graph))
    Nodes.push_back(Obs);
  ASSERT_EQ(Nodes.size(), 4u);
  EXPECT_EQ(Nodes[0], Entry);
  EXPECT_EQ(Nodes[1], Load);
  EXPECT_EQ(Nodes[2], Store);
  EXPECT_EQ(Nodes[3], Exit);

  llvm::SmallVector<const Observation *, 2> EntryChildren;
  for (const Observation *Obs : llvm::children<const Observation *>(Entry))
    EntryChildren.push_back(Obs);
  ASSERT_EQ(EntryChildren.size(), 1u);
  EXPECT_EQ(EntryChildren[0], Load);

  llvm::SmallVector<const Observation *, 2> LoadParents;
  for (const Observation *Obs :
       llvm::inverse_children<const Observation *>(Load))
    LoadParents.push_back(Obs);
  ASSERT_EQ(LoadParents.size(), 2u);
  EXPECT_EQ(LoadParents[0], Entry);
  EXPECT_EQ(LoadParents[1], Store);

  llvm::SmallVector<ObsEdgeKind, 2> LoadChildEdgeKinds;
  for (ObservationEdge Edge : llvm::children_edges<const Observation *>(Load))
    LoadChildEdgeKinds.push_back(Edge.getKind());
  ASSERT_EQ(LoadChildEdgeKinds.size(), 2u);
  EXPECT_EQ(LoadChildEdgeKinds[0], ObsEdgeKind::PostDom);
  EXPECT_EQ(LoadChildEdgeKinds[1], ObsEdgeKind::Dom);

  const MemoryAccess *Access = &Load->accesses().front();
  EXPECT_EQ(llvm::GraphTraits<const MemoryAccess *>::getEntryNode(Access),
            Access);
  EXPECT_EQ(llvm::GraphTraits<const MemoryAccess *>::child_begin(Access),
            llvm::GraphTraits<const MemoryAccess *>::child_end(Access));
}

TEST(MAIRGraph, TracksMemoryAccessesByBasePointer) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
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
    MemoryAccess *Access = Use.getUser();
    ASSERT_NE(Access, nullptr);
    EXPECT_EQ(Use.get(), PObject);
    EXPECT_EQ(Access->getBase(), &P);
    EXPECT_EQ(&Access->getMemoryObject(), PObject);
    ++PUses;
  }
  EXPECT_EQ(PUses, 2u);

  unsigned PUsers = 0;
  for (MemoryAccess &Access : PObject->users()) {
    EXPECT_EQ(&Access.getMemoryObject(), PObject);
    ++PUsers;
  }
  EXPECT_EQ(PUsers, 2u);

  const MAIRGraph &ConstGraph = Graph;
  const MemoryObject *ConstQObject = ConstGraph.getMemoryObject(Q);
  ASSERT_EQ(ConstQObject, QObject);
  unsigned QUses = 0;
  for (const MemoryAccess &Access : ConstQObject->users()) {
    EXPECT_EQ(Access.getBase(), &Q);
    EXPECT_EQ(&Access.getMemoryObject(), QObject);
    ++QUses;
  }
  EXPECT_EQ(QUses, 1u);

  Observation *Store = Graph.find("ma2");
  ASSERT_NE(Store, nullptr);
  Graph.erase(*Store);

  ASSERT_TRUE(Graph.verify());
  PUses = 0;
  for (MemoryAccess &Access : PObject->users()) {
    EXPECT_EQ(Access.getBase(), &P);
    EXPECT_EQ(&Access.getMemoryObject(), PObject);
    ++PUses;
  }
  EXPECT_EQ(PUses, 1u);

  Observation *QLoad = Graph.find("ma3");
  ASSERT_NE(QLoad, nullptr);
  ASSERT_FALSE(QLoad->empty());
  MemoryAccess &QAccess = *QLoad->begin();
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
  MAIRGraph Source = createGraphWithExplicitAnalyses(F);
  MemoryObject *ObjectBeforeMove = Source.getMemoryObject(P);
  ASSERT_NE(ObjectBeforeMove, nullptr);

  MAIRGraph Moved(std::move(Source));

  ASSERT_TRUE(Moved.verify());
  MemoryObject *ObjectAfterMove = Moved.getMemoryObject(P);
  ASSERT_EQ(ObjectAfterMove, ObjectBeforeMove);
  unsigned Users = 0;
  for (MemoryAccess &Access : ObjectAfterMove->users()) {
    EXPECT_EQ(&Access.getMemoryObject(), ObjectAfterMove);
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

  MemoryAccess Access = MemoryAccess::fromInstruction(Load, M->getDataLayout());

  ASSERT_NE(Access.getContiguousRegion(), nullptr);
  EXPECT_EQ(Access.getContiguousRegion()->getBase(), &P);
  EXPECT_EQ(Access.getContiguousRegion()->getOffset(),
            AbstractValue::constant(8));
  EXPECT_EQ(Access.getContiguousRegion()->getSize(),
            AbstractValue::constant(4));
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

  MemoryAccess Access = MemoryAccess::fromInstruction(Load, M->getDataLayout());

  ASSERT_NE(Access.getContiguousRegion(), nullptr);
  EXPECT_EQ(Access.getContiguousRegion()->getBase(), &P);
  EXPECT_EQ(Access.getContiguousRegion()->getOffset(),
            AbstractValue::symbol(GEP));
  EXPECT_EQ(Access.getContiguousRegion()->getSize(),
            AbstractValue::constant(4));
}

TEST(MAIRGraph, EraseObservationMaintainsEdges) {
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
  auto &Entry = Graph.createBBObservation(BB, true);
  auto &Load = Graph.createInstObservation(BB, LoadInst);
  Graph.addEdge(Entry, Load, ObsEdgeKind::Dom);
  Graph.addEdge(Entry, Load, ObsEdgeKind::PostDom);
  Graph.addEdge(Load, Load, ObsEdgeKind::Dom);
  Graph.addEdge(Load, Load, ObsEdgeKind::PostDom);

  Graph.erase(Load);

  EXPECT_TRUE(Graph.verify());
  EXPECT_TRUE(Entry.children().empty());
  EXPECT_EQ(Graph.size(), 1u);
}

TEST(MAIRGraph, FunctionLocalObservationKindsVerify) {
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
  auto &Entry = Graph.createBBObservation(BB, true);
  auto &LoadObs = Graph.createInstObservation(BB, Load);
  Graph.addEdge(Entry, LoadObs, ObsEdgeKind::Dom);

  EXPECT_TRUE(Graph.verify());
  EXPECT_TRUE(Entry.isBasicBlockEntrySite());
  EXPECT_TRUE(LoadObs.isMemoryInstSite());
  ASSERT_EQ(LoadObs.accesses().size(), 1u);
  EXPECT_EQ(LoadObs.accesses().front().getParent(), &LoadObs);
  EXPECT_EQ(&Entry.children().front().getNode(), &LoadObs);
}

TEST(MAIRGraph, CreateFromBuildsLinearMemoryObservationChain) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);

  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 4u);
  const Observation *Entry = Graph.find("bs0");
  const Observation *Load = Graph.find("ma1");
  const Observation *Store = Graph.find("ma2");
  const Observation *Exit = Graph.find("be3");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  ASSERT_NE(Exit, nullptr);
  EXPECT_TRUE(Entry->isBasicBlockEntrySite());
  EXPECT_TRUE(Load->isMemoryInstSite());
  EXPECT_TRUE(Store->isMemoryInstSite());
  EXPECT_TRUE(Exit->isBasicBlockExitSite());
  ASSERT_EQ(Load->accesses().size(), 1u);
  ASSERT_EQ(Store->accesses().size(), 1u);
  EXPECT_EQ(Load->accesses().front().getKind(), AccessKind::Read);
  EXPECT_EQ(Store->accesses().front().getKind(), AccessKind::Write);
  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *Load));
  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *Load, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Exit));
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Entry));
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Entry, ObsEdgeKind::PostDom));
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);

  ASSERT_TRUE(Graph.verify());
  const Observation *Entry = findBlockEntry(Graph, "entry");
  const Observation *Then = findBlockEntry(Graph, "then");
  const Observation *Else = findBlockEntry(Graph, "else");
  const Observation *Merge = findBlockEntry(Graph, "merge");
  const Observation *EntryExit = findBlockExit(Graph, "entry");
  const Observation *ThenExit = findBlockExit(Graph, "then");
  const Observation *ElseExit = findBlockExit(Graph, "else");
  const Observation *MergeExit = findBlockExit(Graph, "merge");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Then, nullptr);
  ASSERT_NE(Else, nullptr);
  ASSERT_NE(Merge, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  ASSERT_NE(ThenExit, nullptr);
  ASSERT_NE(ElseExit, nullptr);
  ASSERT_NE(MergeExit, nullptr);

  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Then, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Else, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Merge, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Then, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Else, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Merge, ObsEdgeKind::Dom));

  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *ThenExit, ObsEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *ElseExit, ObsEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *EntryExit, ObsEdgeKind::PostDom));
  EXPECT_FALSE(
      hasSuccessor(Graph, *MergeExit, *ThenExit, ObsEdgeKind::PostDom));
  EXPECT_FALSE(
      hasSuccessor(Graph, *MergeExit, *ElseExit, ObsEdgeKind::PostDom));
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<ForwardCallBarrierTrait>(F);
  const Observation *Entry = findBlockEntry(Graph, "entry");
  const Observation *EntryExit = findBlockExit(Graph, "entry");
  const Observation *Normal = findBlockEntry(Graph, "normal");
  const Observation *Unwind = findBlockEntry(Graph, "unwind");
  const Observation *Merge = findBlockEntry(Graph, "merge");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  ASSERT_NE(Normal, nullptr);
  ASSERT_NE(Unwind, nullptr);
  ASSERT_NE(Merge, nullptr);

  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *EntryExit, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Normal, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Unwind, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Entry, *Merge, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *EntryExit, *Normal, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *EntryExit, *Unwind, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *EntryExit, *Merge, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *EntryExit, ObsEdgeKind::PostDom));
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
  MAIRGraph Graph =
      createGraphWithExplicitAnalyses<BackwardCallBarrierTrait>(F);
  const Observation *Entry = findBlockEntry(Graph, "entry");
  const Observation *EntryExit = findBlockExit(Graph, "entry");
  const Observation *Left = findBlockEntry(Graph, "left");
  const Observation *Right = findBlockEntry(Graph, "right");
  const Observation *Merge = findBlockEntry(Graph, "merge");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  ASSERT_NE(Left, nullptr);
  ASSERT_NE(Right, nullptr);
  ASSERT_NE(Merge, nullptr);

  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Entry, ObsEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Left, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Right, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *EntryExit, *Merge, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, *Merge, *EntryExit, ObsEdgeKind::PostDom));
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<CallBarrierTrait>(F);

  ASSERT_TRUE(Graph.verify());
  const Observation *Load = Graph.find("ma1");
  const Observation *Store = Graph.find("ma2");
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<CallBarrierTrait>(F);

  ASSERT_TRUE(Graph.verify());
  const Observation *Load = Graph.find("ma1");
  const Observation *Store = Graph.find("ma2");
  const Observation *Exit = Graph.find("be3");
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<UnmatchedBarrierTrait>(F);

  ASSERT_TRUE(Graph.verify());
  const Observation *Load = Graph.find("ma1");
  const Observation *Store = Graph.find("ma2");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Store, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Load));
}

TEST(MAIRGraph, ModelsMemoryAccessPropertiesAndMemoryIntrinsics) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  ASSERT_TRUE(Graph.verify());

  unsigned NumObservations = 0;
  unsigned NumAccesses = 0;
  unsigned NumAtomic = 0;
  unsigned NumVolatile = 0;
  unsigned NumCallLike = 0;
  unsigned NumMasked = 0;
  const Observation *Memcpy = nullptr;
  for (const Observation &Obs : Graph) {
    if (!Obs.isMemoryInstSite())
      continue;
    ++NumObservations;
    NumAccesses += Obs.size();
    if (const auto *MI = llvm::dyn_cast<llvm::MemCpyInst>(&Obs.getAnchor()))
      Memcpy = &Obs;
    for (const MemoryAccess &Access : Obs) {
      NumAtomic += Access.isAtomic();
      NumVolatile += Access.isVolatile();
      NumCallLike += Access.isCallLike();
      NumMasked += Access.isMasked();
    }
  }

  EXPECT_EQ(NumObservations, 10u);
  EXPECT_EQ(NumAccesses, 13u);
  EXPECT_EQ(NumAtomic, 5u);
  EXPECT_EQ(NumVolatile, 3u);
  EXPECT_EQ(NumCallLike, 7u);
  EXPECT_EQ(NumMasked, 2u);

  ASSERT_NE(Memcpy, nullptr);
  ASSERT_EQ(Memcpy->size(), 2u);
  auto It = Memcpy->begin();
  const MemoryAccess &Dest = *It++;
  const MemoryAccess &Source = *It;
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<CallBarrierTrait>(F);
  ASSERT_TRUE(Graph.verify());
  const Observation *Load = Graph.find("ma1");
  const Observation *Memcpy = Graph.find("ma2");
  const Observation *Store = Graph.find("ma3");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Memcpy, nullptr);
  ASSERT_NE(Store, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Memcpy));
  EXPECT_TRUE(hasSuccessor(Graph, *Memcpy, *Load));
  EXPECT_TRUE(hasSuccessor(Graph, *Memcpy, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Memcpy));
}

TEST(MAIRGraph, AtomicAccessObservationRemainsABarrierEndpoint) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<AtomicBarrierTrait>(F);
  ASSERT_TRUE(Graph.verify());
  const Observation *Load = Graph.find("ma1");
  const Observation *Atomic = Graph.find("ma2");
  const Observation *Store = Graph.find("ma3");
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Atomic, nullptr);
  ASSERT_NE(Store, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Atomic));
  EXPECT_TRUE(hasSuccessor(Graph, *Atomic, *Load));
  EXPECT_FALSE(hasSuccessor(Graph, *Atomic, *Store));
  EXPECT_FALSE(hasSuccessor(Graph, *Store, *Atomic));
}

TEST(MAIRGraphPass, SpecialAccessesRemainAtTheirOriginalObservation) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation *Memset = Graph.find("ma1");
  ASSERT_NE(Memset, nullptr);
  ASSERT_EQ(Memset->size(), 1u);
  EXPECT_TRUE(Memset->begin()->isCallLike());

  MAIRGraphAnalysisManager AM;
  llvm::PassInstrumentationCallbacks PIC;
  registerMAIRGraphAnalyses(AM, &PIC);
  SimplifyGraphPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.find("ma1"), Memset);
  ASSERT_EQ(Memset->size(), 1u);
  EXPECT_TRUE(Memset->begin()->isCallLike());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraph, CreateFromHandlesFunctionWithoutMemoryAccesses) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);

  EXPECT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 2u);
  const Observation *Entry = Graph.find("bs0");
  const Observation *Exit = Graph.find("be1");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Exit, nullptr);
  EXPECT_TRUE(Entry->isBasicBlockEntrySite());
  EXPECT_TRUE(Exit->isBasicBlockExitSite());
  EXPECT_TRUE(Entry->accesses().empty());
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
  MAIRGraph Explicit = createGraphWithExplicitAnalyses(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
  const llvm::DominatorTree &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(F);
  const llvm::PostDominatorTree &PDT =
      FAM.getResult<llvm::PostDominatorTreeAnalysis>(F);
  const llvm::LoopInfo &LI = FAM.getResult<llvm::LoopAnalysis>(F);
  MAIRGraph FromFAM = MAIRGraph::build(F, FAM);

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

TEST(MAIRLoopAnalysis, MapsObservationsToLoopsAndRepresentatives) {
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);
  ASSERT_TRUE(Loops.isAvailable());

  const Observation *BodyObs = findMemoryObservation(Graph, "body");
  ASSERT_NE(BodyObs, nullptr);
  ASSERT_EQ(BodyObs->size(), 1u);
  llvm::Loop *L = Loops.getLoopFor(*BodyObs);
  ASSERT_NE(L, nullptr);
  EXPECT_EQ(L->getHeader()->getName(), "header");
  EXPECT_TRUE(Loops.isLoopInvariant(*BodyObs->begin(), *L));
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);

  const Observation *ThenObs = findMemoryObservation(Graph, "then");
  ASSERT_NE(ThenObs, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*ThenObs);
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);

  const MemoryAccess *OutermostAccess = nullptr;
  const MemoryAccess *InnerOnlyAccess = nullptr;
  const MemoryAccess *VariantAccess = nullptr;
  for (const Observation &Obs : Graph)
    for (const MemoryAccess &Access : Obs)
      for (const llvm::Instruction *I : Access.getInstructions()) {
        if (I->getName() == "outermost")
          OutermostAccess = &Access;
        else if (I->getName() == "inner.only")
          InnerOnlyAccess = &Access;
        else if (I->getName() == "variant")
          VariantAccess = &Access;
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

TEST(MAIRLoopAnalysis, RejectsNonLoopAndMultiExitObservations) {
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  const MAIRLoopInfo &Loops = AM.getResult<mair::LoopAnalysis>(Graph);

  const Observation *Outside = findMemoryObservation(Graph, "entry");
  ASSERT_NE(Outside, nullptr);
  EXPECT_EQ(Loops.getLoopFor(*Outside), nullptr);

  const Observation *Inside = findMemoryObservation(Graph, "header");
  ASSERT_NE(Inside, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*Inside);
  ASSERT_NE(L, nullptr);
}

TEST(MAIRLoopAnalysis, ComputesExactObservationExecutionCounts) {
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  MAIRLoopInfo Loops(Graph, FAM);
  const Observation *Header = findMemoryObservation(Graph, "header");
  const Observation *Body = findMemoryObservation(Graph, "body");
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

TEST(MAIRLoopAnalysis, RejectsIrreducibleRepeatedSite) {
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  MAIRLoopInfo Loops(Graph, FAM);
  const llvm::BasicBlock *Latch = nullptr;
  for (const llvm::BasicBlock &BB : F)
    if (BB.getName() == "latch")
      Latch = &BB;
  ASSERT_NE(Latch, nullptr);
  const Observation &LatchEntry = Graph.getBBEntryRepresentative(*Latch);
  llvm::Loop *L = Loops.getLoopFor(LatchEntry);
  ASSERT_NE(L, nullptr);

  EXPECT_FALSE(Loops.executesOncePerRelevantIteration(LatchEntry, *L));
  EXPECT_EQ(Loops.getExactExecutionCountSCEV(LatchEntry, *L), nullptr);
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  MAIRLoopInfo Loops(Graph, FAM);
  const Observation *Header = findMemoryObservation(Graph, "header");
  ASSERT_NE(Header, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*Header);
  ASSERT_NE(L, nullptr);

  const auto *Count = llvm::dyn_cast_or_null<llvm::SCEVConstant>(
      Loops.getExactExecutionCountSCEV(*Header, *L));
  ASSERT_NE(Count, nullptr);
  EXPECT_EQ(Count->getAPInt(), llvm::APInt::getOneBitSet(65, 64));
}

TEST(MAIRLoopAnalysis, DescribesDedicatedAndSharedUniqueExits) {
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

  auto Check = [&](llvm::Function &F, bool ExpectedDedicated) {
    llvm::PassInstrumentationCallbacks PIC;
    llvm::FunctionAnalysisManager FAM;
    registerLoopTestAnalyses(FAM, PIC);
    MAIRGraph Graph = MAIRGraph::build(F, FAM);
    MAIRLoopInfo Loops(Graph, FAM);
    const Observation *Header = findBlockEntry(Graph, "header");
    ASSERT_NE(Header, nullptr);
    llvm::Loop *L = Loops.getLoopFor(*Header);
    ASSERT_NE(L, nullptr);
    std::optional<MAIRLoopExit> Exit = Loops.describeUniqueExit(*L);
    ASSERT_TRUE(Exit.has_value());
    EXPECT_EQ(Exit->Exiting->getName(), "header");
    EXPECT_EQ(Exit->Exit->getName(), "exit");
    EXPECT_EQ(Exit->IsDedicated, ExpectedDedicated);
  };

  Check(*M->getFunction("dedicated"), true);
  Check(*M->getFunction("shared"), false);
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
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
  const Observation &MergeEntry = Graph.getBBEntryRepresentative(*Merge);

  EXPECT_TRUE(Loops.isValueAvailableAt(*F.getArg(0), MergeEntry.getSite()));
  EXPECT_TRUE(Loops.isValueAvailableAt(*MergedValue, MergeEntry.getSite()));
  EXPECT_FALSE(Loops.isValueAvailableAt(*LeftValue, MergeEntry.getSite()));
  EXPECT_TRUE(Loops.isValueAvailableAt(*LeftValue,
                                       ObservationSite::edge(*Left, *Merge)));
  EXPECT_FALSE(Loops.isValueAvailableAt(*LeftValue,
                                        ObservationSite::edge(*Right, *Merge)));
}

TEST(MAIRGraphPass, PeriodicCombineRejectsMismatchedProvenanceRecurrences) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, ptr %q) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %qi = getelementptr i8, ptr %q, i64 %i
  %pv = load i8, ptr %pi, align 1
  %qv = load i8, ptr %qi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  Observation *Source = nullptr;
  Observation *OtherSource = nullptr;
  llvm::Instruction *PLoad = nullptr;
  llvm::Instruction *QLoad = nullptr;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  for (Observation &Obs : Graph) {
    if (!Obs.isMemoryInstSite() || Obs.getBasicBlock()->getName() != "header")
      continue;
    const llvm::Instruction *I = &Obs.getAnchor();
    if (I->getName() == "pv") {
      Source = &Obs;
      PLoad = const_cast<llvm::Instruction *>(I);
    } else if (I->getName() == "qv") {
      OtherSource = &Obs;
      QLoad = const_cast<llvm::Instruction *>(I);
    }
  }
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(OtherSource, nullptr);
  ASSERT_NE(PLoad, nullptr);
  ASSERT_NE(QLoad, nullptr);
  while (!Source->empty())
    Source->erase(Source->begin());
  while (!OtherSource->empty())
    OtherSource->erase(OtherSource->begin());

  const llvm::Argument *P = F.getArg(0);
  MemoryAccess Combined(
      {PLoad, QLoad},
      AbstractAccess(P, AbstractValue::constant(0), AbstractValue::constant(1)),
      AccessKind::Read);
  Source->addAccess(Combined);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  LoopPeriodicObservationCombinePass Pass;
  Pass.run(Graph, AM);

  EXPECT_EQ(Source->size(), 1u);
  const Observation *Exit = findBlockEntry(Graph, "exit");
  ASSERT_NE(Exit, nullptr);
  EXPECT_TRUE(Exit->empty());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, InvariantMotionCanPreserveSingleInstructionProvenance) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %a = load i32, ptr %p, align 4
  %b = load i32, ptr %p, align 4
  %more = icmp slt i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  %next = add i32 %i, 1
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopInvariantObservationMotionPass Pass(/*MergeEquivalentAccesses=*/false);
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *EntryExit = findBlockExit(Graph, "entry");
  ASSERT_NE(EntryExit, nullptr);
  ASSERT_EQ(EntryExit->size(), 2u);
  for (const MemoryAccess &Access : *EntryExit)
    EXPECT_EQ(Access.getInstructions().size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, InvariantMotionDoesNotReprocessEdgeObservations) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %header
header:
  %value = load i32, ptr %p, align 4
  %done = icmp eq i32 %value, 0
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &Header = *std::next(F.begin());
  llvm::BasicBlock &Exit = *std::next(F.begin(), 2);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  Observation *Source = findMutableMemoryObservation(Graph, "header");
  Observation &Preheader = Graph.getBBExitRepresentative(F.getEntryBlock());
  ASSERT_NE(Source, nullptr);
  ASSERT_EQ(Source->size(), 1u);

  Observation &Edge = Graph.createEdgeObservation(Header, Exit);
  Edge.addAccess(*Source->begin());
  Source->erase(Source->begin());
  Graph.addEdge(Preheader, Edge, ObsEdgeKind::Dom);
  Graph.addEdge(Edge, Preheader, ObsEdgeKind::PostDom);

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopInvariantObservationMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Edge.size(), 1u);
  EXPECT_TRUE(Preheader.empty());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, InvariantMotionHoistsBeforeFollowingCallBarrier) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier() readonly

define void @f(ptr %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %header ]
  %value = load i32, ptr %p, align 4
  call void @barrier()
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build<CallBarrierTrait>(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopInvariantObservationMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *Source = findMemoryObservation(Graph, "header");
  const Observation *EntryExit = findBlockExit(Graph, "entry");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  EXPECT_TRUE(Source->empty());
  EXPECT_EQ(EntryExit->size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, InvariantMotionRequiresPreheaderPostDomAncestry) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier() readonly

define void @f(ptr %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %header ]
  call void @barrier()
  %value = load i32, ptr %p, align 4
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build<BackwardCallBarrierTrait>(F, FAM);
  const Observation *Source = findMemoryObservation(Graph, "header");
  const Observation *Preheader = findBlockExit(Graph, "entry");
  const Observation *ExitEntry = findBlockEntry(Graph, "exit");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(Preheader, nullptr);
  ASSERT_NE(ExitEntry, nullptr);
  EXPECT_TRUE(Graph.isAncestor(*Preheader, *Source, ObsEdgeKind::Dom));
  EXPECT_FALSE(Graph.isAncestor(*Source, *Preheader, ObsEdgeKind::PostDom));
  EXPECT_TRUE(Graph.isAncestor(*ExitEntry, *Source, ObsEdgeKind::PostDom));

  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopInvariantObservationMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  EXPECT_TRUE(Source->empty());
  EXPECT_TRUE(Preheader->empty());
  EXPECT_TRUE(ExitEntry->empty());
  const Observation *GuardedExit = nullptr;
  for (const Observation &Obs : Graph)
    if (Obs.hasExecutionGuard() &&
        Obs.getBasicBlock() == ExitEntry->getBasicBlock()) {
      GuardedExit = &Obs;
      break;
    }
  ASSERT_NE(GuardedExit, nullptr);
  EXPECT_EQ(GuardedExit->size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, InvariantMotionStopsAtSurroundingCallBarriers) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier() readonly

define void @f(ptr %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %header ]
  call void @barrier()
  %value = load i32, ptr %p, align 4
  call void @barrier()
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build<CallBarrierTrait>(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopInvariantObservationMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *Source = findMemoryObservation(Graph, "header");
  const Observation *EntryExit = findBlockExit(Graph, "entry");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(EntryExit, nullptr);
  EXPECT_EQ(Source->size(), 1u);
  EXPECT_TRUE(EntryExit->empty());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, InvariantMotionSinksWhenOnlyExitIsAnAncestor) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier() readonly

define void @f(ptr %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %header ]
  call void @barrier()
  %value = load i32, ptr %p, align 4
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build<ForwardCallBarrierTrait>(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopInvariantObservationMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *Source = findMemoryObservation(Graph, "header");
  const Observation *ExitEntry = findBlockEntry(Graph, "exit");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(ExitEntry, nullptr);
  EXPECT_TRUE(Source->empty());
  EXPECT_TRUE(ExitEntry->empty());
  const Observation *GuardedExit = nullptr;
  for (const Observation &Obs : Graph)
    if (Obs.hasExecutionGuard() && Obs.getBasicBlock() &&
        Obs.getBasicBlock()->getName() == "exit") {
      GuardedExit = &Obs;
      break;
    }
  ASSERT_NE(GuardedExit, nullptr);
  EXPECT_EQ(GuardedExit->size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, InvariantMotionFallsBackToInnerInvariantLoop) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier() readonly

define void @f(ptr %p) {
entry:
  br label %outer.header
outer.header:
  %oi = phi i32 [ 0, %entry ], [ %oi.next, %outer.latch ]
  call void @barrier()
  br label %inner.header
inner.header:
  %ii = phi i32 [ 0, %outer.header ], [ %ii.next, %inner.header ]
  %value = load i32, ptr %p, align 4
  %ii.next = add i32 %ii, 1
  %inner.done = icmp eq i32 %ii.next, 2
  br i1 %inner.done, label %outer.latch, label %inner.header
outer.latch:
  call void @barrier()
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
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build<CallBarrierTrait>(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopInvariantObservationMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *Source = findMemoryObservation(Graph, "inner.header");
  const Observation *OuterPreheader = findBlockExit(Graph, "entry");
  const Observation *InnerPreheader = findBlockExit(Graph, "outer.header");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(OuterPreheader, nullptr);
  ASSERT_NE(InnerPreheader, nullptr);
  EXPECT_TRUE(Source->empty());
  EXPECT_TRUE(OuterPreheader->empty());
  EXPECT_EQ(InnerPreheader->size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, PeriodicCombineCanPreserveSingleInstructionProvenance) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %a = load i8, ptr %pi, align 1
  %b = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopPeriodicObservationCombinePass Pass(
      /*MergeEquivalentAccesses=*/false);
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *ExitEntry = findBlockEntry(Graph, "exit");
  ASSERT_NE(ExitEntry, nullptr);
  ASSERT_EQ(ExitEntry->size(), 2u);
  for (const MemoryAccess &Access : *ExitEntry)
    EXPECT_EQ(Access.getInstructions().size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, PeriodicCombineDoesNotCrossCallBarrier) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier() readonly

define void @f(ptr %p) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %pi = getelementptr i8, ptr %p, i64 %i
  %value = load i8, ptr %pi, align 1
  call void @barrier()
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build<CallBarrierTrait>(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopPeriodicObservationCombinePass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *Source = findMemoryObservation(Graph, "header");
  const Observation *ExitEntry = findBlockEntry(Graph, "exit");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(ExitEntry, nullptr);
  EXPECT_EQ(Source->size(), 1u);
  EXPECT_TRUE(ExitEntry->empty());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, PeriodicCombineSinksAfterPrecedingCallBarrier) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier() readonly

define void @f(ptr %p) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  call void @barrier()
  %pi = getelementptr i8, ptr %p, i64 %i
  %value = load i8, ptr %pi, align 1
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
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
  registerLoopTestAnalyses(FAM, PIC);
  MAIRGraph Graph = MAIRGraph::build<CallBarrierTrait>(F, FAM);
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC, &FAM);

  LoopPeriodicObservationCombinePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *Source = findMemoryObservation(Graph, "header");
  const Observation *ExitEntry = findBlockEntry(Graph, "exit");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(ExitEntry, nullptr);
  EXPECT_TRUE(Source->empty());
  EXPECT_EQ(ExitEntry->size(), 1u);
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
  auto &Load = Graph.createInstObservation(BB, LoadInst);
  Graph.addEdge(Load, Load, ObsEdgeKind::Dom);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Load.size(), 1u);
}

TEST(MAIRGraphPass, RedundantObservationEliminatesWithinObservation) {
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
  auto &Obs = Graph.createInstObservation(BB, FirstLoad);
  MemoryAccess Second =
      MemoryAccess::fromInstruction(SecondLoad, M->getDataLayout());
  Obs.addAccess(Second);
  Graph.addEdge(Obs, Obs, ObsEdgeKind::Dom);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Obs.size(), 1u);
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
  Observation &Obs = Graph.createInstObservation(BB, NarrowLoad);
  Obs.addAccess(MemoryAccess::fromInstruction(WideLoad, M->getDataLayout()));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Obs.size(), 1u);
  const AbstractAccess *Region = Obs.begin()->getContiguousRegion();
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
  Observation &Obs = Graph.createInstObservation(BB, Read);
  Obs.addAccess(MemoryAccess::fromInstruction(Write, M->getDataLayout()));
  ASSERT_LT(Obs.begin()->getLevel(), std::next(Obs.begin())->getLevel());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Obs.size(), 1u);
  EXPECT_TRUE(Obs.begin()->isWrite());
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
  auto &Obs = Graph.createInstObservation(BB, WideLoad);
  MemoryAccess Atomic =
      MemoryAccess::fromInstruction(AtomicLoad, M->getDataLayout());
  MemoryAccess Narrow =
      MemoryAccess::fromInstruction(NarrowLoad, M->getDataLayout());
  Obs.addAccess(Atomic);
  Obs.addAccess(Narrow);

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Obs.size(), 2u);
  EXPECT_FALSE(Obs.begin()->isAtomic());
  EXPECT_TRUE(std::next(Obs.begin())->isAtomic());
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
  auto &Obs = Graph.createInstObservation(BB, WideLoad);
  MemoryAccess Store =
      MemoryAccess::fromInstruction(NarrowStore, M->getDataLayout());
  Obs.addAccess(Store);

  EXPECT_LT(Obs.begin()->getLevel(), std::next(Obs.begin())->getLevel());
  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  RedundantObservationEliminationPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Obs.size(), 2u);
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
  auto &P = Graph.createInstObservation(BB, PLoad);
  auto &Q = Graph.createInstObservation(BB, QLoad);
  auto &Merge = Graph.createInstObservation(BB, RedundantPLoad);
  Graph.addEdge(P, Merge, ObsEdgeKind::Dom);
  Graph.addEdge(Q, Merge, ObsEdgeKind::PostDom);

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
  auto &ReadWrite = Graph.createInstObservation(BB, FirstLoad);
  ReadWrite.begin()->mergeKind(AccessKind::Write);
  auto &Read = Graph.createInstObservation(BB, SecondLoad);
  auto &Write = Graph.createInstObservation(BB, StoreInst);
  EXPECT_GT(ReadWrite.begin()->getLevel(), Write.begin()->getLevel());
  EXPECT_GT(Write.begin()->getLevel(), Read.begin()->getLevel());
  Graph.addEdge(ReadWrite, Read, ObsEdgeKind::Dom);
  Graph.addEdge(Read, Write, ObsEdgeKind::Dom);

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
  auto &Read = Graph.createInstObservation(BB, Load);
  auto &Write = Graph.createInstObservation(BB, Store);
  Graph.addEdge(Read, Write, ObsEdgeKind::Dom);

  MemoryAccess Copy = MemoryAccess::fromInstruction(Load, M->getDataLayout());
  EXPECT_FALSE(Copy.mergeWith(*Write.begin()));
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
  auto &Plain = Graph.createInstObservation(BB, PlainLoad);
  auto &Atomic = Graph.createInstObservation(BB, AtomicLoad);
  Graph.addEdge(Plain, Atomic, ObsEdgeKind::Dom);

  MemoryAccess Copy =
      MemoryAccess::fromInstruction(PlainLoad, M->getDataLayout());
  EXPECT_FALSE(Copy.mergeWith(*Atomic.begin()));

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
  auto &Entry = Graph.createBBObservation(BB, true);
  auto &Load = Graph.createInstObservation(BB, LoadInst);
  auto &Store = Graph.createInstObservation(BB, StoreInst);
  auto &Exit = Graph.createBBObservation(BB, false);
  Graph.addEdge(Entry, Load, ObsEdgeKind::Dom);
  Graph.addEdge(Load, Store, ObsEdgeKind::Dom);
  Graph.addEdge(Store, Load, ObsEdgeKind::PostDom);
  Graph.addEdge(Store, Exit, ObsEdgeKind::Dom);

  MAIRGraphAnalysisManager AM;
  SimplifyGraphPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 3u);
  EXPECT_EQ(Graph.find("ma1"), nullptr);
  const Observation *Merged = Graph.find("ma2");
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->accesses().size(), 2u);
  EXPECT_TRUE(hasSuccessor(Graph, Entry, *Merged, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merged, Exit, ObsEdgeKind::Dom));
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  EXPECT_EQ(Graph.find("ma1"), nullptr);
  const Observation *Merged = Graph.find("ma2");
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(&Merged->getAnchor(), Second);
  ASSERT_EQ(Merged->size(), 2u);
  auto Access = Merged->begin();
  EXPECT_EQ(Access->getInstruction(), First);
  EXPECT_EQ((++Access)->getInstruction(), Second);
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
  auto &Load = Graph.createInstObservation(BB, LoadInst);
  auto &Store = Graph.createInstObservation(BB, StoreInst);
  Graph.addEdge(Load, Store, ObsEdgeKind::Dom);
  Graph.addEdge(Store, Load, ObsEdgeKind::Dom);

  MAIRGraphAnalysisManager AM;
  SimplifyGraphPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_TRUE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.size(), 2u);
  EXPECT_NE(Graph.find("ma0"), nullptr);
  EXPECT_NE(Graph.find("ma1"), nullptr);
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
  auto &A = Graph.createInstObservation(BB, LoadInst);
  auto &B = Graph.createInstObservation(BB, StoreInst);
  auto &C = Graph.createBBObservation(BB, false);
  auto &D = Graph.createBBObservation(BB, false);
  auto &E = Graph.createBBObservation(BB, false);
  Graph.addEdge(A, B, ObsEdgeKind::Dom);
  Graph.addEdge(B, A, ObsEdgeKind::PostDom);
  Graph.addEdge(C, B, ObsEdgeKind::PostDom);
  Graph.addEdge(B, D, ObsEdgeKind::Dom);
  Graph.addEdge(B, E, ObsEdgeKind::PostDom);

  MAIRGraphAnalysisManager AM;
  SimplifyGraphPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Graph.find("ma0"), nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, C, B, ObsEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, B, D, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, B, E, ObsEdgeKind::PostDom));
  EXPECT_FALSE(hasSuccessor(Graph, B, C));
  EXPECT_FALSE(hasSuccessor(Graph, B, E, ObsEdgeKind::Dom));
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
  Observation &Earlier = Graph.createInstObservation(BB, LoadInst);
  Observation &Later = Graph.createInstObservation(BB, StoreInst);
  Observation &External = Graph.createBBObservation(BB, false);
  Graph.addEdge(Earlier, Later, ObsEdgeKind::Dom);
  Graph.addEdge(Later, Earlier, ObsEdgeKind::PostDom);
  Graph.addEdge(Later, External, ObsEdgeKind::Dom);
  Graph.addEdge(Later, External, ObsEdgeKind::PostDom);
  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.size(), 2u);
  EXPECT_TRUE(Graph.hasEdge(Later, External, ObsEdgeKind::Dom));
  EXPECT_TRUE(Graph.hasEdge(Later, External, ObsEdgeKind::PostDom));
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
  Observation &Source = Graph.createInstObservation(BB, BB.front());
  Observation &Target = Graph.createBBObservation(BB, false);
  Graph.addEdge(Source, Target, ObsEdgeKind::Dom);
  Graph.addEdge(Source, Target, ObsEdgeKind::PostDom);

  EXPECT_TRUE(Source.hasChild(Target, ObsEdgeKind::Dom));
  EXPECT_TRUE(Source.hasChild(Target, ObsEdgeKind::PostDom));
  EXPECT_TRUE(Target.hasParent(Source, ObsEdgeKind::Dom));
  EXPECT_TRUE(Target.hasParent(Source, ObsEdgeKind::PostDom));

  Graph.removeEdge(Source, Target, ObsEdgeKind::Dom);
  EXPECT_FALSE(Source.hasChild(Target, ObsEdgeKind::Dom));
  EXPECT_TRUE(Source.hasChild(Target, ObsEdgeKind::PostDom));
  EXPECT_FALSE(Target.hasParent(Source, ObsEdgeKind::Dom));
  EXPECT_TRUE(Target.hasParent(Source, ObsEdgeKind::PostDom));
  EXPECT_TRUE(Graph.hasEdge(Source, Target, ObsEdgeKind::PostDom));
  EXPECT_TRUE(Graph.verify());
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());

  ASSERT_TRUE(Graph.verify());
  for (const llvm::BasicBlock &BB : F) {
    const Observation &Entry = Graph.getBBEntryRepresentative(BB);
    const Observation &Exit = Graph.getBBExitRepresentative(BB);
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation *Outside = findMutableMemoryObservation(Graph, "entry");
  Observation *Inside = findMutableMemoryObservation(Graph, "loop");
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation *Before = findMutableMemoryObservation(Graph, "entry");
  Observation *After = findMutableMemoryObservation(Graph, "exit");
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation *Outer = findMutableMemoryObservation(Graph, "outer.header");
  Observation *Inner = findMutableMemoryObservation(Graph, "inner.header");
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation *Before = findMutableMemoryObservation(Graph, "outer.header");
  Observation *After = findMutableMemoryObservation(Graph, "outer.latch");
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation *First = findMutableMemoryObservation(Graph, "loop.header");
  Observation *Second = findMutableMemoryObservation(Graph, "loop.latch");
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  Graph.addEdge(*First, *Second, ObsEdgeKind::Dom);
  Graph.addEdge(*Second, *First, ObsEdgeKind::PostDom);
  const size_t InitialSize = Graph.size();
  const std::string FirstName = First->getName().str();
  ASSERT_TRUE(Graph.verify());

  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  EXPECT_EQ(Graph.size(), InitialSize - 1);
  EXPECT_EQ(Graph.find(FirstName), nullptr);
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
  Observation &Movable = Graph.createInstObservation(BB, MovableInst);
  Observation &Pinned = Graph.createInstObservation(BB, PinnedInst);
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
  Observation &Pinned = Graph.createInstObservation(BB, PinnedInst);
  Observation &Movable = Graph.createInstObservation(BB, MovableInst);
  ASSERT_FALSE(Pinned.begin()->isMovable());
  ASSERT_TRUE(Movable.begin()->isMovable());

  expectSimplifyGraphPreservesMutualPair(Graph, Pinned, Movable);
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
  Observation &Entry = Graph.createBBObservation(BB, true);
  Observation &Exit = Graph.createBBObservation(BB, false);
  ASSERT_EQ(&Graph.getBBEntryRepresentative(BB), &Entry);
  ASSERT_EQ(&Graph.getBBExitRepresentative(BB), &Exit);
  const MAIRGraph &ConstGraph = Graph;
  ASSERT_EQ(&ConstGraph.getBBEntryRepresentative(BB), &Entry);
  ASSERT_EQ(&ConstGraph.getBBExitRepresentative(BB), &Exit);

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

TEST(MAIRGraphPass, CommonMAMotionDoesNotSearchNeighborDescendants) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  const llvm::Value &Alloca = *F.getEntryBlock().begin();
  const MemoryObject *Object = Graph.getMemoryObject(Alloca);
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  Observation &TargetBefore = Graph.getBBExitRepresentative(F.getEntryBlock());
  Observation &LeftBoundary =
      Graph.getBBEntryRepresentative(*std::next(F.begin()));
  const Observation *LeftAccess = findMemoryObservation(Graph, "left");
  ASSERT_NE(LeftAccess, nullptr);
  EXPECT_TRUE(LeftBoundary.hasParent(TargetBefore, ObsEdgeKind::Dom));
  EXPECT_FALSE(LeftAccess->hasParent(TargetBefore, ObsEdgeKind::Dom));

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const Observation *Target = findBlockExit(Graph, "entry");
  ASSERT_NE(Target, nullptr);
  EXPECT_TRUE(Target->empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
}

TEST(MAIRGraphPass,
     CommonMAMotionHoistsBoundaryAccessesAndPreservesProvenance) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  moveMemoryObservationAccessesTo(Graph, "left",
                                  Graph.getBBEntryRepresentative(Left));
  moveMemoryObservationAccessesTo(Graph, "right",
                                  Graph.getBBEntryRepresentative(Right));
  const llvm::Value &Alloca = *F.getEntryBlock().begin();
  const MemoryObject *Object = Graph.getMemoryObject(Alloca);
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  ASSERT_TRUE(Graph.verify());
  const Observation *Target = findBlockExit(Graph, "entry");
  ASSERT_NE(Target, nullptr);
  ASSERT_EQ(Target->size(), 1u);
  EXPECT_EQ(Target->begin()->getInstructions().size(), 2u);
  EXPECT_TRUE(llvm::is_contained(Target->begin()->getInstructions(), LeftLoad));
  EXPECT_TRUE(
      llvm::is_contained(Target->begin()->getInstructions(), RightLoad));
  EXPECT_EQ(llvm::range_size(Object->users()), 1u);
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
}

TEST(MAIRGraphPass, CommonMAMotionRequiresCommonTargetParent) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation &Target = Graph.getBBExitRepresentative(Dispatch);
  Observation &EntryExit = Graph.getBBExitRepresentative(Entry);
  Observation &LeftBoundary = Graph.getBBEntryRepresentative(Left);
  Observation &RightBoundary = Graph.getBBEntryRepresentative(Right);
  moveMemoryObservationAccessesTo(Graph, "left", LeftBoundary);
  moveMemoryObservationAccessesTo(Graph, "right", RightBoundary);
  ASSERT_TRUE(LeftBoundary.hasParent(Target, ObsEdgeKind::Dom));
  ASSERT_FALSE(RightBoundary.hasParent(Target, ObsEdgeKind::Dom));
  ASSERT_TRUE(RightBoundary.hasParent(EntryExit, ObsEdgeKind::Dom));
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  ASSERT_EQ(llvm::range_size(Object->users()), 2u);
  ASSERT_TRUE(Graph.verify());

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_TRUE(Target.empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonMAMotionRequiresCommonTargetParentForSink) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation &Target = Graph.getBBEntryRepresentative(Merge);
  Observation &LeftBoundary = Graph.getBBExitRepresentative(Left);
  Observation &RightBoundary = Graph.getBBExitRepresentative(Right);
  moveMemoryObservationAccessesTo(Graph, "left", LeftBoundary);
  moveMemoryObservationAccessesTo(Graph, "right", RightBoundary);
  ASSERT_TRUE(LeftBoundary.hasParent(Target, ObsEdgeKind::PostDom));
  ASSERT_FALSE(RightBoundary.hasParent(Target, ObsEdgeKind::PostDom));
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  ASSERT_EQ(llvm::range_size(Object->users()), 2u);
  ASSERT_TRUE(Graph.verify());

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_TRUE(Target.empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonMAMotionHoistsPointerFromDominatingBlock) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  llvm::BasicBlock &Left = *std::next(F.begin(), 2);
  llvm::BasicBlock &Right = *std::next(F.begin(), 3);
  moveMemoryObservationAccessesTo(Graph, "left",
                                  Graph.getBBEntryRepresentative(Left));
  moveMemoryObservationAccessesTo(Graph, "right",
                                  Graph.getBBEntryRepresentative(Right));
  CommonMAMotionPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  const Observation *Target = findBlockExit(Graph, "dispatch");
  ASSERT_NE(Target, nullptr);
  ASSERT_EQ(Target->size(), 1u);
  EXPECT_EQ(Target->begin()->getInstructions().size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonMAMotionAssertsUnavailableCommonAccess) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation &LeftBoundary = Graph.getBBEntryRepresentative(Left);
  Observation &RightBoundary = Graph.getBBEntryRepresentative(Right);
  moveMemoryObservationAccessesTo(Graph, "left", LeftBoundary);
  ASSERT_EQ(LeftBoundary.size(), 1u);
  RightBoundary.addAccess(*LeftBoundary.begin());
  ASSERT_TRUE(Graph.verify());

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
#ifndef NDEBUG
  EXPECT_DEATH_IF_SUPPORTED((void)Pass.run(Graph, AM),
                            "common access operands must be available");
#endif
}

TEST(MAIRGraphPass, CommonMAMotionSinksToEntryAndPreservesProvenance) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  llvm::BasicBlock &Left = *std::next(F.begin(), 3);
  llvm::BasicBlock &Right = *std::next(F.begin(), 4);
  moveMemoryObservationAccessesTo(Graph, "left",
                                  Graph.getBBExitRepresentative(Left));
  moveMemoryObservationAccessesTo(Graph, "right",
                                  Graph.getBBExitRepresentative(Right));
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const Observation *Target = findBlockEntry(Graph, "merge");
  ASSERT_NE(Target, nullptr);
  ASSERT_EQ(Target->size(), 1u);
  EXPECT_EQ(Target->begin()->getInstructions().size(), 2u);
  EXPECT_TRUE(llvm::is_contained(Target->begin()->getInstructions(), LeftLoad));
  EXPECT_TRUE(
      llvm::is_contained(Target->begin()->getInstructions(), RightLoad));
  EXPECT_EQ(llvm::range_size(Object->users()), 1u);
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
}

TEST(MAIRGraphPass, CommonMAMotionDoesNotSearchPredecessorDescendants) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  const MemoryObject *Object = Graph.getMemoryObject(*F.getArg(0));
  ASSERT_NE(Object, nullptr);
  ASSERT_EQ(llvm::range_size(Object->users()), 2u);

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_TRUE(Graph.getBBEntryRepresentative(*std::next(F.begin(), 3)).empty());
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonMAMotionHoistsAfterForwardBarrierInTarget) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<CallBarrierTrait>(F);
  llvm::BasicBlock &Left = *std::next(F.begin());
  llvm::BasicBlock &Right = *std::next(F.begin(), 2);
  moveMemoryObservationAccessesTo(Graph, "left",
                                  Graph.getBBEntryRepresentative(Left));
  moveMemoryObservationAccessesTo(Graph, "right",
                                  Graph.getBBEntryRepresentative(Right));
  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;

  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const Observation *Target = findBlockExit(Graph, "entry");
  ASSERT_NE(Target, nullptr);
  EXPECT_EQ(Target->size(), 1u);
}

TEST(MAIRGraphPass, CommonMAMotionHoistsBeforeForwardBarrierInNeighbor) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<CallBarrierTrait>(F);
  moveMemoryObservationAccessesTo(Graph, "left",
                                  Graph.getBBEntryRepresentative(Left));
  moveMemoryObservationAccessesTo(Graph, "right",
                                  Graph.getBBEntryRepresentative(Right));

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.getBBExitRepresentative(F.getEntryBlock()).size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonMAMotionSinksAfterBackwardBarrierInPredecessor) {
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<CallBarrierTrait>(F);
  moveMemoryObservationAccessesTo(Graph, "left",
                                  Graph.getBBExitRepresentative(Left));
  moveMemoryObservationAccessesTo(Graph, "right",
                                  Graph.getBBExitRepresentative(Right));

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.getBBEntryRepresentative(Merge).size(), 1u);
  EXPECT_TRUE(Graph.verify());
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
  Observation &EntryRep = Graph.createBBObservation(Entry, true);
  Observation &ExitRep = Graph.createBBObservation(Exit, true);
  Observation &Edge = Graph.createEdgeObservation(
      Entry, Exit, AnyProvenanceExecutedGuard({&Load}));

  ASSERT_TRUE(Graph.verify());
  EXPECT_TRUE(Edge.isEdgeSite());
  EXPECT_EQ(Edge.getEdgeFrom(), &Entry);
  EXPECT_EQ(Edge.getEdgeTo(), &Exit);
  EXPECT_TRUE(Edge.hasExecutionGuard());
  EXPECT_EQ(&Graph.getBBEntryRepresentative(Entry), &EntryRep);
  EXPECT_EQ(&Graph.getBBEntryRepresentative(Exit), &ExitRep);
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
  CrossFunction.createEdgeObservation(FEntry, G.getEntryBlock());
  std::string CrossFunctionError;
  llvm::raw_string_ostream CrossFunctionOS(CrossFunctionError);
  EXPECT_FALSE(CrossFunction.verify(&CrossFunctionOS));
  EXPECT_NE(CrossFunctionError.find("edge site crosses functions"),
            std::string::npos);

  MAIRGraph MissingEdge(F);
  MissingEdge.createEdgeObservation(Left, Right);
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
  ObservationGuard AB = AnyProvenanceExecutedGuard({A, B});
  ObservationGuard BA = AnyProvenanceExecutedGuard({B, A});
  ObservationGuard AOnly = AnyProvenanceExecutedGuard({A});
  ObservationGuard DuplicateA = AnyProvenanceExecutedGuard({A, A});

  EXPECT_EQ(AB, BA);
  EXPECT_NE(AB, AOnly);
  EXPECT_NE(DuplicateA, AB);
  EXPECT_NE(AB, DuplicateA);
  EXPECT_NE(AB, ObservationGuard(AlwaysGuard{}));
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
  Observation &Obs = Graph.createBBObservation(F.getEntryBlock(), false);
  MAIRScalarExpr Start = Graph.createValueExpr(Base);
  MAIRScalarExpr One = Graph.createConstantExpr(I64, llvm::APInt(64, 1));
  Obs.addAccess(MemoryAccess({}, PeriodicRegion(&Base, Start, One, One, One),
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
  Observation &Anchor = Graph.createBBObservation(F.getEntryBlock(), true);
  Graph.createObservation(Anchor.getSite(),
                          AnyProvenanceExecutedGuard({&Load, &Load}));
  Graph.createObservation(
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
  Observation &Representative = Graph.createBBObservation(BB, true);
  Observation &Guarded = Graph.createObservation(
      Representative.getSite(), AnyProvenanceExecutedGuard({&Load}));

  ASSERT_TRUE(Graph.verify());
  EXPECT_EQ(Guarded.getSite(), Representative.getSite());
  EXPECT_NE(Guarded.getGuard(), Representative.getGuard());
  EXPECT_EQ(&Graph.getBBEntryRepresentative(BB), &Representative);
}

TEST(MAIRSchema, ObservationSortCacheInvalidatesWhenAccessIsAdded) {
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
  Observation &Obs = Graph.createBBObservation(BB, true);

  auto MakeAccess = [&](uint64_t Offset) {
    return MemoryAccess(
        {&Load},
        MemoryRegion(AbstractAccess(&P, AbstractValue::constant(Offset),
                                    AbstractValue::constant(4))),
        AccessKind::Read);
  };
  auto CollectOffsets = [&]() {
    llvm::SmallVector<uint64_t, 4> Offsets;
    for (const MemoryAccess &Access : Obs) {
      const AbstractAccess *Region = Access.getContiguousRegion();
      assert(Region && Region->getOffset().isConstant());
      Offsets.push_back(Region->getOffset().getConstant());
    }
    return Offsets;
  };

  Obs.addAccess(MakeAccess(8));
  Obs.addAccess(MakeAccess(0));
  Obs.sort(compareTestAccesses);

  llvm::SmallVector<uint64_t, 4> Offsets = CollectOffsets();
  ASSERT_EQ(Offsets.size(), 2u);
  EXPECT_EQ(Offsets[0], 0u);
  EXPECT_EQ(Offsets[1], 8u);

  Obs.sort(compareTestAccesses);
  Obs.addAccess(MakeAccess(4));
  Obs.sort(compareTestAccesses);

  Offsets = CollectOffsets();
  ASSERT_EQ(Offsets.size(), 3u);
  EXPECT_EQ(Offsets[0], 0u);
  EXPECT_EQ(Offsets[1], 4u);
  EXPECT_EQ(Offsets[2], 8u);

  auto ReverseCompare = [](const MemoryAccess &LHS, const MemoryAccess &RHS) {
    return compareTestAccesses(RHS, LHS);
  };
  Obs.sort(ReverseCompare);
  Offsets = CollectOffsets();
  EXPECT_EQ(Offsets[0], 8u);
  EXPECT_EQ(Offsets[1], 4u);
  EXPECT_EQ(Offsets[2], 0u);
  Obs.sort(compareTestAccesses);
  Offsets = CollectOffsets();
  EXPECT_EQ(Offsets[0], 0u);
  EXPECT_EQ(Offsets[1], 4u);
  EXPECT_EQ(Offsets[2], 8u);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, ObservationSortCacheInvalidatesWhenAccessKeyChanges) {
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
  Observation &Obs = Graph.createBBObservation(BB, true);

  auto MakeAccess = [&](uint64_t Offset, uint64_t Size) {
    return MemoryAccess(
        {&Load},
        MemoryRegion(AbstractAccess(&P, AbstractValue::constant(Offset),
                                    AbstractValue::constant(Size))),
        AccessKind::Read);
  };

  MemoryAccess &Left = Obs.addAccess(MakeAccess(0, 4));
  MemoryAccess &Right = Obs.addAccess(MakeAccess(4, 4));
  Obs.sort(compareTestAccesses);
  ASSERT_EQ(&*Obs.begin(), &Left);
  ASSERT_TRUE(Right.mergeWith(Left));

  Obs.sort(compareTestAccesses);
  EXPECT_EQ(&*Obs.begin(), &Right);
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, ObservationSortCacheInvalidatesWhenAccessLevelChanges) {
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
  Observation &Obs = Graph.createBBObservation(BB, true);
  AbstractAccess Region(&P, AbstractValue::constant(0),
                        AbstractValue::constant(4));

  MemoryAccess &Read =
      Obs.addAccess(MemoryAccess({&Load}, Region, AccessKind::Read));
  MemoryAccess &Write =
      Obs.addAccess(MemoryAccess({&Load}, Region, AccessKind::Write));
  Obs.sort(compareTestAccesses);
  ASSERT_EQ(&*Obs.begin(), &Write);

  Read.mergeKind(AccessKind::Write);
  Obs.sort(compareTestAccesses);
  EXPECT_EQ(&*Obs.begin(), &Read);
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
  Observation &Obs = Graph.createBBObservation(BB, true);
  MAIRScalarExpr Start = Graph.createValueExpr(P);
  MAIRScalarExpr Stride = Graph.createConstantExpr(*llvm::Type::getInt64Ty(Ctx),
                                                   llvm::APInt(64, 8));
  MAIRScalarExpr AccessSize = Graph.createConstantExpr(
      *llvm::Type::getInt64Ty(Ctx), llvm::APInt(64, 4));
  MAIRScalarExpr ZeroTrips = Graph.createConstantExpr(
      *llvm::Type::getInt64Ty(Ctx), llvm::APInt(64, 0));
  PeriodicRegion Region(&P, Start, Stride, AccessSize, ZeroTrips);
  Obs.addAccess(MemoryAccess({&Load}, MemoryRegion(Region), AccessKind::Read));

  ASSERT_TRUE(Graph.verify());
  ASSERT_TRUE(Obs.begin()->isPeriodic());
  ASSERT_NE(Obs.begin()->getPeriodicRegion(), nullptr);
  EXPECT_EQ(Obs.begin()->getBase(), &P);

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
  Observation &Obs = Graph.createBBObservation(F.getEntryBlock(), true);

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
  Obs.addAccess(MemoryAccess(
      {&Load}, MemoryRegion(PeriodicRegion(&P, Start, Stride, Size, Trips)),
      AccessKind::Read));

  ASSERT_TRUE(Graph.verify());
  std::string Text = Graph.toString();
  EXPECT_NE(Text.find("start=(%p + 4)"), std::string::npos);
  EXPECT_NE(Text.find("stride=sext("), std::string::npos);
  EXPECT_NE(Text.find("access-size="), std::string::npos);
  EXPECT_NE(Text.find("trips=zext(%n to i64)"), std::string::npos);
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
  Observation &Obs = Graph.createBBObservation(F.getEntryBlock(), true);
  MAIRScalarExpr Start = Graph.createValueExpr(P);
  MAIRScalarExpr ZeroStride = Graph.createConstantExpr(I64, llvm::APInt(64, 0));
  MAIRScalarExpr Size = Graph.createConstantExpr(I64, llvm::APInt(64, 4));
  MAIRScalarExpr ForeignTrips =
      Other.createConstantExpr(I64, llvm::APInt(64, 3));
  PeriodicRegion Region(&P, Start, ZeroStride, Size, ForeignTrips);
  Obs.addAccess(MemoryAccess({&Load}, MemoryRegion(Region), AccessKind::Read));

  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("scalar expression belongs to another graph"),
            std::string::npos);
  EXPECT_NE(Error.find("periodic region has zero stride"), std::string::npos);
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
  Observation &Obs = Graph.createBBObservation(F.getEntryBlock(), true);
  MAIRScalarExpr IntegerStart =
      Graph.createConstantExpr(I64, llvm::APInt(64, 0));
  MAIRScalarExpr PointerStride = Graph.createValueExpr(P);
  MAIRScalarExpr ZeroSize = Graph.createConstantExpr(I64, llvm::APInt(64, 0));
  MAIRScalarExpr TripSource = Graph.createConstantExpr(I64, llvm::APInt(64, 2));
  MAIRScalarExpr Trips =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::Truncate, I64, TripSource);
  Obs.addAccess(
      MemoryAccess({&Load},
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
  Observation &Obs = Graph.createBBObservation(F.getEntryBlock(), true);
  MAIRScalarExpr Start = Graph.createValueExpr(P);
  MAIRScalarExpr C4 = Graph.createConstantExpr(I64, llvm::APInt(64, 4));
  MAIRScalarExpr InvalidStride = Graph.createBinaryExpr(
      MAIRScalarExpr::Kind::Add, I64, MAIRScalarExpr(), C4);
  Obs.addAccess(MemoryAccess(
      {&Load}, MemoryRegion(PeriodicRegion(&P, Start, InvalidStride, C4, C4)),
      AccessKind::Read));

  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(Graph.verify(&OS));
  EXPECT_NE(Error.find("invalid scalar expression"), std::string::npos);
}

TEST(MAIRSchema, EqualPeriodicRegionsRequireSameAccessKindToMerge) {
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
  llvm::Argument &P = *F.arg_begin();
  llvm::BasicBlock &BB = F.getEntryBlock();
  llvm::Instruction *AInst = &BB.front();
  llvm::Instruction *BInst = &*std::next(BB.begin());
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
  MemoryAccess A(
      {AInst}, MemoryRegion(PeriodicRegion(&P, StartA, StrideA, SizeA, TripsA)),
      AccessKind::Read);
  MemoryAccess B(
      {BInst}, MemoryRegion(PeriodicRegion(&P, StartB, StrideB, SizeB, TripsB)),
      AccessKind::Write);

  EXPECT_FALSE(A.mergeWith(B));
  EXPECT_EQ(A.getKind(), AccessKind::Read);
  EXPECT_EQ(A.getInstructions().size(), 1u);
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
  MemoryAccess Access({&Load}, MemoryRegion(std::move(Region)),
                      AccessKind::Read);

  EXPECT_TRUE(Access.isMasked());
  MAIRGraph Graph(F);
  Observation &Obs = Graph.createBBObservation(F.getEntryBlock(), true);
  MemoryAccess &Inserted = Obs.addAccess(Access);
  EXPECT_TRUE(Inserted.isMasked());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRSchema, ContainingContiguousMergeKeepsCoveringInstructionFirst) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %wide = load i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  %narrow = load i32, ptr %p4, align 4
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
  MemoryAccess WideAccess(
      {Wide},
      MemoryRegion(AbstractAccess(&P, AbstractValue::constant(0),
                                  AbstractValue::constant(8))),
      AccessKind::Read);
  MemoryAccess NarrowAccess(
      {Narrow},
      MemoryRegion(AbstractAccess(&P, AbstractValue::constant(4),
                                  AbstractValue::constant(4))),
      AccessKind::Read);

  EXPECT_TRUE(WideAccess.mergeWith(NarrowAccess));
  EXPECT_EQ(WideAccess.getInstructions().size(), 2u);
  EXPECT_EQ(WideAccess.getInstructions().front(), Wide);
  EXPECT_EQ(WideAccess.getInstructions().back(), Narrow);

  MemoryAccess Reversed(
      {Narrow},
      MemoryRegion(AbstractAccess(&P, AbstractValue::constant(4),
                                  AbstractValue::constant(4))),
      AccessKind::Read);
  MemoryAccess Covering(
      {Wide},
      MemoryRegion(AbstractAccess(&P, AbstractValue::constant(0),
                                  AbstractValue::constant(8))),
      AccessKind::Read);
  EXPECT_TRUE(Reversed.mergeWith(Covering));
  EXPECT_EQ(Reversed.getInstructions().size(), 2u);
  EXPECT_EQ(Reversed.getInstructions().front(), Wide);
  EXPECT_EQ(Reversed.getInstructions().back(), Narrow);
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
  Graph.createBBObservation(Entry, true);
  Graph.createEdgeObservation(Entry, Exit, AnyProvenanceExecutedGuard({&Load}));

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
  MemoryAccess &ReadAccess = *Graph.createInstObservation(BB, Read).begin();
  MemoryAccess &WriteAccess = *Graph.createInstObservation(BB, Write).begin();

  EXPECT_EQ(ReadAccess.getLevel(), WriteAccess.getLevel());
  EXPECT_TRUE(ReadAccess.isMergeable());
  EXPECT_TRUE(WriteAccess.isMergeable());
  EXPECT_TRUE(ReadAccess.covers(WriteAccess));
  EXPECT_TRUE(WriteAccess.covers(ReadAccess));
  EXPECT_FALSE(ReadAccess.isMovable());
  EXPECT_FALSE(WriteAccess.isMovable());
  EXPECT_TRUE(ReadAccess.isDeletable());
  EXPECT_TRUE(WriteAccess.isDeletable());
  EXPECT_TRUE(ReadAccess.mergeWith(WriteAccess));
  EXPECT_TRUE(ReadAccess.isReadWrite());
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
  MemoryAccess &Normal = *Graph.createInstObservation(BB, *It++).begin();
  MemoryAccess &Atomic = *Graph.createInstObservation(BB, *It++).begin();
  MemoryAccess &Volatile = *Graph.createInstObservation(BB, *It++).begin();
  MemoryAccess &CallLike = *Graph.createInstObservation(BB, *It++).begin();

  EXPECT_TRUE(Normal.isMergeable());
  EXPECT_TRUE(Normal.isMovable());
  EXPECT_TRUE(Normal.isDeletable());
  EXPECT_EQ(Normal.getLevel(), 1u);

  EXPECT_FALSE(Atomic.isMergeable());
  EXPECT_FALSE(Atomic.isMovable());
  EXPECT_FALSE(Atomic.isDeletable());
  EXPECT_FALSE(Volatile.isMergeable());
  EXPECT_FALSE(Volatile.isMovable());
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
  MemoryAccess &ReadAccess = *Graph.createInstObservation(BB, Read).begin();
  MemoryAccess &WriteAccess = *Graph.createInstObservation(BB, Write).begin();

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
  Observation &Available = Graph.createInstObservation(BB, First);
  Observation &Required = Graph.createInstObservation(BB, Second);
  Graph.addEdge(Available, Required, ObsEdgeKind::Dom);

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
    Observation &Available = Graph.createInstObservation(BB, First);
    Observation &Required = Graph.createInstObservation(BB, Second);
    Graph.addEdge(Available, Required, ObsEdgeKind::Dom);

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
  Observation &Earlier = Graph.createInstObservation(BB, First);
  Observation &Later = Graph.createInstObservation(BB, Second);
  Graph.addEdge(Later, Earlier, ObsEdgeKind::PostDom);

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
    MemoryAccess &First = *Graph.createInstObservation(BB, BB.front()).begin();
    MemoryAccess &Second =
        *Graph.createInstObservation(BB, *std::next(BB.begin())).begin();
    EXPECT_TRUE(First.isCallLike());
    EXPECT_TRUE(Second.isCallLike());
    EXPECT_FALSE(First.isMergeable());
    EXPECT_FALSE(Second.isMergeable());
    EXPECT_FALSE(First.isDeletable());
    EXPECT_FALSE(Second.isDeletable());
    EXPECT_FALSE(First.mergeWith(Second));
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
  MemoryAccess &Wide = *Graph.createInstObservation(BB, BB.front()).begin();
  MemoryAccess &Narrow =
      *Graph.createInstObservation(BB, *std::next(BB.begin())).begin();

  EXPECT_TRUE(Wide.isMasked());
  EXPECT_FALSE(Wide.isCallLike());
  ASSERT_NE(Wide.getMaskedRegion(), nullptr);
  EXPECT_TRUE(Wide.covers(Narrow));
  EXPECT_FALSE(Narrow.covers(Wide))
      << "narrow=" << Narrow.toString() << " wide=" << Wide.toString();
  EXPECT_TRUE(Narrow.mergeWith(Wide));
  EXPECT_TRUE(Narrow.covers(Wide));
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
  MemoryAccess &First = *Graph.createInstObservation(BB, *It++).begin();
  MemoryAccess &Same = *Graph.createInstObservation(BB, *It++).begin();
  MemoryAccess &Different = *Graph.createInstObservation(BB, *It).begin();

  EXPECT_TRUE(First.covers(Same));
  EXPECT_FALSE(First.covers(Different))
      << "first=" << First.toString() << " different=" << Different.toString();
  EXPECT_TRUE(First.mergeWith(Same));
  EXPECT_FALSE(First.mergeWith(Different))
      << "first=" << First.toString() << " different=" << Different.toString();
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
  Observation &Obs = Graph.createBBObservation(BB, true);

  auto MakeAccess = [&](uint64_t Offset) {
    return MemoryAccess(
        {&Load},
        MemoryRegion(AbstractAccess(&P, AbstractValue::constant(Offset),
                                    AbstractValue::unknown())),
        AccessKind::Read);
  };

  Obs.addAccess(MakeAccess(8));
  Obs.addAccess(MakeAccess(0));
  Obs.addAccess(MakeAccess(8));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(Obs.size(), 2u);

  auto It = Obs.begin();
  ASSERT_NE(It->getContiguousRegion(), nullptr);
  EXPECT_EQ(It->getContiguousRegion()->getOffset(), AbstractValue::constant(0));
  ++It;
  ASSERT_NE(It->getContiguousRegion(), nullptr);
  EXPECT_EQ(It->getContiguousRegion()->getOffset(), AbstractValue::constant(8));
  EXPECT_EQ(It->getInstructions().size(), 2u);
  EXPECT_TRUE(Graph.verify());
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
  llvm::Instruction &PX = *It++;
  llvm::Instruction &LowerStore = *It++;
  ++It;
  llvm::Instruction &UpperStore = *It++;
  ++It;
  llvm::Instruction &GappedStore = *It;

  MemoryAccess Lower =
      MemoryAccess::fromInstruction(LowerStore, M->getDataLayout());
  MemoryAccess Upper =
      MemoryAccess::fromInstruction(UpperStore, M->getDataLayout());

  ASSERT_EQ(Lower.getBase(), &P);
  ASSERT_EQ(Upper.getBase(), &P);
  ASSERT_TRUE(Lower.mergeWith(Upper));
  const AbstractAccess *Merged = Lower.getContiguousRegion();
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->getOffset(), AbstractValue::symbol(PX));
  EXPECT_EQ(Merged->getSize(), AbstractValue::constant(8));
  EXPECT_TRUE(Lower.isWrite());

  MemoryAccess ReversedUpper =
      MemoryAccess::fromInstruction(UpperStore, M->getDataLayout());
  MemoryAccess ReversedLower =
      MemoryAccess::fromInstruction(LowerStore, M->getDataLayout());
  ASSERT_TRUE(ReversedUpper.mergeWith(ReversedLower));
  Merged = ReversedUpper.getContiguousRegion();
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->getOffset(), AbstractValue::symbol(PX));
  EXPECT_EQ(Merged->getSize(), AbstractValue::constant(8));

  MemoryAccess GappedLower =
      MemoryAccess::fromInstruction(LowerStore, M->getDataLayout());
  MemoryAccess GappedUpper =
      MemoryAccess::fromInstruction(GappedStore, M->getDataLayout());
  EXPECT_FALSE(GappedLower.mergeWith(GappedUpper));
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
  Observation &Obs = Graph.createBBObservation(BB, true);
  Obs.addAccess(MemoryAccess::fromInstruction(LowerStore, M->getDataLayout()));
  Obs.addAccess(MemoryAccess::fromInstruction(UpperStore, M->getDataLayout()));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(Obs.size(), 1u);

  const AbstractAccess *Merged = Obs.begin()->getContiguousRegion();
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->getBase(), F.arg_begin());
  EXPECT_EQ(Merged->getSize(), AbstractValue::constant(8));
  EXPECT_TRUE(Obs.begin()->isWrite());
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
  Observation &Obs = Graph.createBBObservation(BB, true);

  Obs.addAccess(
      MemoryAccess({&Load},
                   MemoryRegion(AbstractAccess(&P, AbstractValue::constant(0),
                                               AbstractValue::constant(8))),
                   AccessKind::Read));
  Obs.addAccess(
      MemoryAccess({&Load},
                   MemoryRegion(AbstractAccess(&P, AbstractValue::constant(4),
                                               AbstractValue::unknown())),
                   AccessKind::Read));
  Obs.addAccess(
      MemoryAccess({&Load},
                   MemoryRegion(AbstractAccess(&P, AbstractValue::constant(8),
                                               AbstractValue::constant(4))),
                   AccessKind::Read));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(Obs.size(), 2u);

  auto It = Obs.begin();
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
  Observation &Obs = Graph.createBBObservation(BB, true);
  Obs.addAccess(MemoryAccess::fromInstruction(Left, M->getDataLayout()));
  Obs.addAccess(MemoryAccess::fromInstruction(Barrier, M->getDataLayout()));
  Obs.addAccess(MemoryAccess::fromInstruction(Right, M->getDataLayout()));

  ASSERT_EQ(Obs.begin()->getLevel(), std::prev(Obs.end())->getLevel());
  ASSERT_NE(Obs.begin()->getLevel(), std::next(Obs.begin())->getLevel());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(Obs.size(), 2u);

  const MemoryAccess &Merged = *Obs.begin();
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
  Observation &Obs = Graph.createBBObservation(BB, true);
  Obs.addAccess(MemoryAccess::fromInstruction(Left, M->getDataLayout()));
  Obs.addAccess(MemoryAccess::fromInstruction(Barrier, M->getDataLayout()));
  Obs.addAccess(MemoryAccess::fromInstruction(Right, M->getDataLayout()));

  ASSERT_TRUE(Obs.begin()->isMergeable());
  ASSERT_FALSE(std::next(Obs.begin())->isMergeable());
  ASSERT_TRUE(std::prev(Obs.end())->isMergeable());

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(Obs.size(), 2u);

  auto Merged = llvm::find_if(
      Obs, [](const MemoryAccess &Access) { return Access.isMergeable(); });
  ASSERT_NE(Merged, Obs.end());
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
  Observation &Obs = Graph.createInstObservation(BB, Low);
  Obs.addAccess(MemoryAccess::fromInstruction(High, M->getDataLayout()));
  Obs.addAccess(MemoryAccess::fromInstruction(Opaque, M->getDataLayout()));

  llvm::PassInstrumentationCallbacks PIC;
  MAIRGraphAnalysisManager AM;
  registerMAIRGraphAnalyses(AM, &PIC);
  LocationMergePass Pass;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_EQ(Obs.size(), 2u);
  const MaskedRegion *Merged = Obs.begin()->getMaskedRegion();
  ASSERT_NE(Merged, nullptr);
  ASSERT_TRUE(Merged->isConstantMask());
  EXPECT_TRUE(Merged->getConstantMask().test(0));
  EXPECT_TRUE(Merged->getConstantMask().test(2));
  const MaskedRegion *Unmerged = std::next(Obs.begin())->getMaskedRegion();
  ASSERT_NE(Unmerged, nullptr);
  EXPECT_FALSE(Unmerged->isComparable());
  EXPECT_TRUE(Graph.verify());
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
  Observation &Anchored = Graph.createBBObservation(Entry, true);
  Observation &DifferentGuard = Graph.createObservation(
      Anchored.getSite(), AnyProvenanceExecutedGuard({&Load}));
  Observation &Edge = Graph.createEdgeObservation(Entry, Exit);
  Graph.addEdge(Anchored, DifferentGuard, ObsEdgeKind::Dom);
  Graph.addEdge(DifferentGuard, Anchored, ObsEdgeKind::PostDom);
  Graph.addEdge(Anchored, Edge, ObsEdgeKind::Dom);
  Graph.addEdge(Edge, Anchored, ObsEdgeKind::PostDom);

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
  Observation &LeftEdge = Graph.createEdgeObservation(Entry, Left);
  Observation &RightEdge = Graph.createEdgeObservation(Entry, Right);
  Graph.addEdge(LeftEdge, RightEdge, ObsEdgeKind::Dom);
  Graph.addEdge(RightEdge, LeftEdge, ObsEdgeKind::PostDom);

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
  Observation &Entry = Graph.createBBObservation(BB, true);
  Observation &Memory = Graph.createInstObservation(BB, Load);
  Observation &Exit = Graph.createBBObservation(BB, false);
  Graph.addEdge(Entry, Memory, ObsEdgeKind::Dom);
  Graph.addEdge(Memory, Entry, ObsEdgeKind::PostDom);
  Graph.addEdge(Memory, Exit, ObsEdgeKind::Dom);
  Graph.addEdge(Exit, Memory, ObsEdgeKind::PostDom);

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
  Observation &Entry = Graph.createBBObservation(BB, true);
  ObservationGuard Guard = AnyProvenanceExecutedGuard({&Load});
  Graph.createObservation(Entry.getSite(), Guard);
  Graph.createObservation(
      ObservationSite::anchored(Load, Observation::SiteKind::MemoryInst),
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
