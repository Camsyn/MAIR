#include "MAIR/MAIR.h"
#include "MAIR/MAIRBarrierAnalysis.h"
#include "MAIR/MAIRDump.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace mair;

namespace {
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

const Observation *findBlockEntry(const MAIRGraph &Graph, llvm::StringRef Name) {
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
  EXPECT_TRUE(Analysis.hasCachedBarrierBetween(Then, Then.getFirstNonPHI(),
                                               Then.getTerminator(), true));
  EXPECT_FALSE(Analysis.hasCachedBarrierBetween(Merge, Merge.getFirstNonPHI(),
                                                Merge.getTerminator(), true));
  EXPECT_TRUE(Analysis.hasForwardBarrierOnAnyEntryPath(&Entry, &Merge));
  EXPECT_TRUE(Analysis.hasBackwardBarrierOnAnyEntryPath(&Merge, &Entry));
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

  EXPECT_TRUE(Analysis.hasForwardBarrierBetween(Entry, Load, Store));
  EXPECT_GT(CountingCallBarrierTrait::ForwardQueries, 0u);
  EXPECT_EQ(CountingCallBarrierTrait::BackwardQueries, 0u);
  unsigned QueriesAfterFirstRange = CountingCallBarrierTrait::ForwardQueries;
  EXPECT_TRUE(Analysis.hasForwardBarrierBetween(Entry, Load, Store));
  EXPECT_EQ(CountingCallBarrierTrait::ForwardQueries, QueriesAfterFirstRange);
  EXPECT_TRUE(Analysis.hasCachedBarrierBetween(Entry, Load, Store, true));
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

  MAIRGraph Graph(F);
  auto &Entry = Graph.createBBObservation(BB, true);
  auto &Obs = Graph.createBBObservation(BB, true);
  Obs.addAccess(
      MemoryAccess(nullptr, llvm::MemoryLocation(), AccessKind::Read));
  Observation Other("o1", nullptr, nullptr);
  Other.addAccess(
      MemoryAccess(nullptr, llvm::MemoryLocation(), AccessKind::Write));
  Obs.mergeAccessesFrom(Other);
  Graph.addEdge(Entry, Obs);

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
  EXPECT_NE(Dump.find("mair graph"), std::string::npos);
  EXPECT_NE(Dump.find("obs bs0 {site=bb-entry"), std::string::npos);
  EXPECT_NE(Dump.find("obs bs1"), std::string::npos);
  EXPECT_NE(Dump.find("succs=[bs1]"), std::string::npos);
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
  Graph.addEdge(Entry, Load);

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
  Graph.addEdge(Entry, LoadObs);

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
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Store));
  EXPECT_TRUE(hasSuccessor(Graph, *Store, *Exit));
  EXPECT_TRUE(hasSuccessor(Graph, *Load, *Entry));
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
  EXPECT_TRUE(hasSuccessor(Graph, *ThenExit, *Merge));
  EXPECT_TRUE(hasSuccessor(Graph, *ElseExit, *Merge));
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
  MAIRGraph Graph =
      createGraphWithExplicitAnalyses<CallBarrierTrait>(F);

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
  MAIRGraph Graph =
      createGraphWithExplicitAnalyses<CallBarrierTrait>(F);

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
  MAIRGraph Explicit =
      createGraphWithExplicitAnalyses(F);
  MAIRGraph FromFAM = createGraphWithFAM(F);

  ASSERT_TRUE(Explicit.verify());
  ASSERT_TRUE(FromFAM.verify());
  EXPECT_EQ(FromFAM.toString(), Explicit.toString());
}
