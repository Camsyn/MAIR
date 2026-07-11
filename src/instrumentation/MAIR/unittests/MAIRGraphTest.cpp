#include "MAIR/Analysis/RedundantObservationAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRBarrierAnalysis.h"
#include "MAIR/MAIRDump.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"

#include "llvm/Analysis/PostDominators.h"
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
  Observation Other("o1", nullptr, nullptr);
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

  AbstractAccess A(&P, AbstractValue::constant(0),
                   AbstractValue::constant(4));
  AbstractAccess B(&P, AbstractValue::constant(4),
                   AbstractValue::constant(4));
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

  MemoryAccess Access =
      MemoryAccess::fromInstruction(Load, M->getDataLayout());

  EXPECT_EQ(Access.getAccess().getBase(), &P);
  EXPECT_EQ(Access.getAccess().getOffset(), AbstractValue::constant(8));
  EXPECT_EQ(Access.getAccess().getSize(), AbstractValue::constant(4));
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

  MemoryAccess Access =
      MemoryAccess::fromInstruction(Load, M->getDataLayout());

  EXPECT_EQ(Access.getAccess().getBase(), &P);
  EXPECT_EQ(Access.getAccess().getOffset(), AbstractValue::symbol(GEP));
  EXPECT_EQ(Access.getAccess().getSize(), AbstractValue::constant(4));
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

  MAIRGraph Graph(F);
  auto &A = Graph.createBBObservation(BB, true);
  auto &B = Graph.createInstObservation(BB, LoadInst);
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
  EXPECT_TRUE(hasSuccessor(Graph, A, E, ObsEdgeKind::Dom));
  EXPECT_FALSE(hasSuccessor(Graph, A, C));
  EXPECT_FALSE(hasSuccessor(Graph, A, E, ObsEdgeKind::PostDom));
  EXPECT_FALSE(hasSuccessor(Graph, D, A));
}
