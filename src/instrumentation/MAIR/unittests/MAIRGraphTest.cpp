#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRBarrierAnalysis.h"
#include "MAIR/MAIRDump.h"
#include "MAIR/Transforms/CommonMAMotionPass.h"
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
  return MAIRGraph::build<BarrierTrait>(F, DT, PDT);
}

template <class BarrierTrait = DefaultBarrierTrait>
MAIRGraph createGraphWithFAM(Function &F) {
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  FAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return llvm::DominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::PostDominatorTreeAnalysis(); });
  return MAIRGraph::build<BarrierTrait>(F, FAM);
}

bool hasSuccessor(const MAIRGraph &Graph, const Observation &From,
                  const Observation &To) {
  for (const Observation *Succ : Graph.successors(From))
    if (Succ == &To)
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
    return llvm::isa<llvm::CallBase>(I);
  }

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
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);

  llvm::Function &F = *M->getFunction("f");
  llvm::BasicBlock &Entry = F.getEntryBlock();
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
  ASSERT_EQ(Graph.successors(Entry).size(), 1u);
  EXPECT_EQ(Graph.successors(Entry)[0], &Obs);
  EXPECT_EQ(Graph.predecessors(Obs)[0], &Entry);
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
  QLoad->erase(QLoad->begin());

  ASSERT_TRUE(Graph.verify());
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

  Graph.erase(Load);

  EXPECT_TRUE(Graph.verify());
  EXPECT_TRUE(Graph.successors(Entry).empty());
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
  EXPECT_EQ(Graph.successors(Entry).front(), &LoadObs);
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

TEST(MAIRGraph, CreateFromUsesDominatorAndPostDominatorEntries) {
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
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Then, nullptr);
  ASSERT_NE(Else, nullptr);
  ASSERT_NE(Merge, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *Then));
  EXPECT_TRUE(hasSuccessor(Graph, *Entry, *Else));
  auto FindExit = [&Graph](llvm::StringRef BBName) -> const Observation * {
    for (const Observation &Obs : Graph)
      if (Obs.isBasicBlockExitSite() && Obs.getBasicBlock() &&
          Obs.getBasicBlock()->getName() == BBName)
        return &Obs;
    return nullptr;
  };
  const Observation *ThenExit = FindExit("then");
  const Observation *ElseExit = FindExit("else");
  ASSERT_NE(ThenExit, nullptr);
  ASSERT_NE(ElseExit, nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *ThenExit, ObsEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merge, *ElseExit, ObsEdgeKind::PostDom));
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
  MAIRGraph FromFAM = createGraphWithFAM(F);

  ASSERT_TRUE(Explicit.verify());
  ASSERT_TRUE(FromFAM.verify());
  EXPECT_EQ(FromFAM.toString(), Explicit.toString());
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
  EXPECT_TRUE(Loops.isSimpleLoop(*L));
  EXPECT_TRUE(Loops.isEveryIterationAccess(*BodyObs->begin(), *L));
  EXPECT_TRUE(Loops.isLoopInvariant(*BodyObs->begin(), *L));
  EXPECT_EQ(Loops.getTopInvariantLoop(*BodyObs->begin(), *L), L);
  EXPECT_EQ(Loops.getPreheaderExit(*L), findBlockExit(Graph, "entry"));
  EXPECT_TRUE(Loops.hasDedicatedExit(*L));
  EXPECT_EQ(Loops.getUniqueExitEntry(*L), findBlockEntry(Graph, "exit"));
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
  EXPECT_TRUE(Loops.isSimpleLoop(*L));
  EXPECT_FALSE(Loops.isEveryIterationAccess(*ThenObs->begin(), *L));
}

TEST(MAIRLoopAnalysis, FindsTopInvariantLoop) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %outer.header
outer.header:
  %oi = phi i32 [ 0, %entry ], [ %oi.next, %outer.latch ]
  br label %inner.header
inner.header:
  %ii = phi i32 [ 0, %outer.header ], [ %ii.next, %inner.header ]
  %v = load i32, ptr %p, align 4
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

  const Observation *InnerObs = findMemoryObservation(Graph, "inner.header");
  ASSERT_NE(InnerObs, nullptr);
  llvm::Loop *Inner = Loops.getLoopFor(*InnerObs);
  ASSERT_NE(Inner, nullptr);
  llvm::Loop *Outer = Inner->getParentLoop();
  ASSERT_NE(Outer, nullptr);
  EXPECT_TRUE(Loops.isSimpleLoop(*Inner));
  EXPECT_TRUE(Loops.isSimpleLoop(*Outer));
  EXPECT_EQ(Loops.getTopInvariantLoop(*InnerObs->begin(), *Inner), Outer);
  EXPECT_TRUE(Loops.executesOncePerRelevantIteration(*InnerObs, *Inner));
  EXPECT_FALSE(Loops.executesOncePerRelevantIteration(*InnerObs, *Outer));
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
  EXPECT_FALSE(Loops.isSimpleLoop(*L));
  EXPECT_FALSE(Loops.hasDedicatedExit(*L));
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
  MAIRLoopInfo Loops(Graph, FAM);
  const llvm::BasicBlock *Latch = nullptr;
  for (const llvm::BasicBlock &BB : F)
    if (BB.getName() == "latch")
      Latch = &BB;
  ASSERT_NE(Latch, nullptr);
  const Observation *LatchEntry = Graph.getBBEntryRepresentative(*Latch);
  ASSERT_NE(LatchEntry, nullptr);
  llvm::Loop *L = Loops.getLoopFor(*LatchEntry);
  ASSERT_NE(L, nullptr);

  EXPECT_FALSE(Loops.isSimpleLoop(*L));
  EXPECT_FALSE(Loops.executesOncePerRelevantIteration(*LatchEntry, *L));
  EXPECT_EQ(Loops.getExactExecutionCountSCEV(*LatchEntry, *L), nullptr);
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
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
    MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
    llvm::PassInstrumentationCallbacks PIC;
    llvm::FunctionAnalysisManager FAM;
    registerLoopTestAnalyses(FAM, PIC);
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
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
  const Observation *MergeEntry = Graph.getBBEntryRepresentative(*Merge);
  ASSERT_NE(MergeEntry, nullptr);

  EXPECT_TRUE(Loops.isValueAvailableAt(*F.getArg(0), MergeEntry->getSite()));
  EXPECT_TRUE(Loops.isValueAvailableAt(*MergedValue, MergeEntry->getSite()));
  EXPECT_FALSE(Loops.isValueAvailableAt(*LeftValue, MergeEntry->getSite()));
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  Observation *Source = nullptr;
  Observation *OtherSource = nullptr;
  llvm::Instruction *PLoad = nullptr;
  llvm::Instruction *QLoad = nullptr;
  for (Observation &Obs : Graph) {
    if (!Obs.isMemoryInstSite() || Obs.getBasicBlock()->getName() != "header")
      continue;
    const llvm::Instruction *I = Obs.getAnchor();
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

  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  registerLoopTestAnalyses(FAM, PIC);
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

TEST(MAIRGraphPass, RedundantObservationDoesNotEliminateWithinObservation) {
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

TEST(MAIRGraphPass, RedundantReadWriteAccessCoversReadAndWrite) {
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
  EXPECT_EQ(Graph.find("ma2"), nullptr);
  const Observation *Merged = Graph.find("ma1");
  ASSERT_NE(Merged, nullptr);
  EXPECT_EQ(Merged->accesses().size(), 2u);
  EXPECT_TRUE(hasSuccessor(Graph, Entry, *Merged, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, *Merged, Exit, ObsEdgeKind::Dom));
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
  EXPECT_EQ(Graph.find("ma1"), nullptr);
  EXPECT_TRUE(hasSuccessor(Graph, C, A, ObsEdgeKind::PostDom));
  EXPECT_TRUE(hasSuccessor(Graph, A, D, ObsEdgeKind::Dom));
  EXPECT_TRUE(hasSuccessor(Graph, A, E, ObsEdgeKind::PostDom));
  EXPECT_FALSE(hasSuccessor(Graph, A, C));
  EXPECT_FALSE(hasSuccessor(Graph, A, E, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, D, A));
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
  Observation &Into = Graph.createInstObservation(BB, LoadInst);
  Observation &From = Graph.createInstObservation(BB, StoreInst);
  Observation &External = Graph.createBBObservation(BB, false);
  Graph.addEdge(Into, From, ObsEdgeKind::Dom);
  Graph.addEdge(From, Into, ObsEdgeKind::PostDom);
  Graph.addEdge(From, External, ObsEdgeKind::Dom);
  Graph.addEdge(From, External, ObsEdgeKind::PostDom);
  SimplifyGraphPass Pass;
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(Pass.run(Graph, AM).areAllPreserved());

  ASSERT_TRUE(Graph.verify());
  ASSERT_EQ(Graph.size(), 2u);
  EXPECT_TRUE(Graph.hasEdge(Into, External, ObsEdgeKind::Dom));
  EXPECT_TRUE(Graph.hasEdge(Into, External, ObsEdgeKind::PostDom));
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
    const Observation *Entry = Graph.getBBEntryRepresentative(BB);
    const Observation *Exit = Graph.getBBExitRepresentative(BB);
    ASSERT_NE(Entry, nullptr);
    ASSERT_NE(Exit, nullptr);
    EXPECT_TRUE(Entry->isBasicBlockEntrySite());
    EXPECT_TRUE(Exit->isBasicBlockExitSite());
    EXPECT_TRUE(Entry->representsBlock(BB));
    EXPECT_TRUE(Exit->representsBlock(BB));
  }
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
  Graph.createBBObservation(BB, false);
  ASSERT_EQ(Graph.getBBEntryRepresentative(BB), &Entry);

  Graph.erase(Entry);

  EXPECT_EQ(Graph.getBBEntryRepresentative(BB), nullptr);
  ASSERT_TRUE(Graph.verify());
}

TEST(MAIRGraphPass, CommonMAMotionHoistsToExitAndPreservesProvenance) {
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
  const llvm::Instruction *LeftLoad = &*std::next(F.begin())->begin();
  const llvm::Instruction *RightLoad = &*std::next(F.begin(), 2)->begin();
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
  const llvm::Value &Alloca = *F.getEntryBlock().begin();
  const MemoryObject *Object = Graph.getMemoryObject(Alloca);
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(llvm::range_size(Object->users()), 2u);

  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;
  llvm::PreservedAnalyses PA = Pass.run(Graph, AM);

  EXPECT_FALSE(PA.areAllPreserved());
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

TEST(MAIRGraphPass, CommonMAMotionSinksToEntryAndPreservesProvenance) {
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
  const llvm::Instruction *LeftLoad = &*std::next(F.begin())->begin();
  const llvm::Instruction *RightLoad = &*std::next(F.begin(), 2)->begin();
  MAIRGraph Graph = createGraphWithExplicitAnalyses(F);
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

TEST(MAIRGraphPass, CommonMAMotionRespectsForwardBarrierInTarget) {
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
  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;

  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const Observation *Target = findBlockExit(Graph, "entry");
  ASSERT_NE(Target, nullptr);
  EXPECT_TRUE(Target->empty());
}

TEST(MAIRGraphPass, CommonMAMotionRespectsBackwardBarrierInPredecessor) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(ptr %p, i1 %cond) {
entry:
  call void @barrier()
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
  MAIRGraph Graph = createGraphWithExplicitAnalyses<CallBarrierTrait>(F);
  MAIRGraphAnalysisManager AM;
  CommonMAMotionPass Pass;

  EXPECT_TRUE(Pass.run(Graph, AM).areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  const Observation *Target = findBlockEntry(Graph, "merge");
  ASSERT_NE(Target, nullptr);
  EXPECT_TRUE(Target->empty());
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
  EXPECT_EQ(Graph.getBBEntryRepresentative(Entry), &EntryRep);
  EXPECT_EQ(Graph.getBBEntryRepresentative(Exit), &ExitRep);
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

  EXPECT_EQ(AB, BA);
  EXPECT_NE(AB, AOnly);
  EXPECT_NE(AB, ObservationGuard(AlwaysGuard{}));
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
  EXPECT_EQ(Graph.getBBEntryRepresentative(BB), &Representative);
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

TEST(MAIRSchema, EqualPeriodicRegionsMergeStructurally) {
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

  EXPECT_TRUE(A.mergeWith(B));
  EXPECT_EQ(A.getKind(), AccessKind::ReadWrite);
  EXPECT_EQ(A.getInstructions().size(), 2u);
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
  EXPECT_EQ(Graph.getBBEntryRepresentative(BB), &Entry);
  EXPECT_EQ(Graph.getBBExitRepresentative(BB), &Exit);
  EXPECT_TRUE(Graph.getBBEntryRepresentative(BB)->isBasicBlockEntrySite());
  EXPECT_TRUE(Graph.getBBExitRepresentative(BB)->isBasicBlockExitSite());
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
      ObservationSite::anchored(&BB, &Load, Observation::SiteKind::MemoryInst),
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
