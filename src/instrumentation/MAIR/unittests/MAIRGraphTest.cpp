#include "MAIR/MAIR.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace __xsan::mair;

TEST(MAIRGraph, BasicConstructionAndDump) {
  MAIRGraph Graph;
  auto &Entry = Graph.createObservation("bb0", nullptr, nullptr,
                                        Observation::SiteKind::BasicBlockEntry);
  auto &Obs = Graph.createObservation("o0", nullptr, nullptr,
                                      Observation::SiteKind::BasicBlockEntry);
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
  dumpPure(Graph, OS);
  OS.flush();
  EXPECT_NE(Dump.find("mair graph"), std::string::npos);
  EXPECT_NE(Dump.find("obs bb0 {site=bb-entry"), std::string::npos);
  EXPECT_NE(Dump.find("obs o0"), std::string::npos);
  EXPECT_NE(Dump.find("succs=[o0]"), std::string::npos);
}

TEST(MAIRGraph, EraseObservationMaintainsEdges) {
  MAIRGraph Graph;
  auto &Entry = Graph.createObservation("bb0", nullptr, nullptr,
                                        Observation::SiteKind::BasicBlockEntry);
  auto &Load = Graph.createObservation("o0", nullptr, nullptr);
  Graph.addEdge(Entry, Load);

  Graph.erase(Load);

  EXPECT_TRUE(Graph.verify());
  EXPECT_TRUE(Graph.successors(Entry).empty());
  EXPECT_EQ(Graph.size(), 1u);
}

TEST(MAIRGraph, VerifyRejectsDetachedMemorySiteWithoutAnchor) {
  MAIRGraph Graph;
  Graph.createObservation("o0", nullptr, nullptr,
                          Observation::SiteKind::MemoryInst);

  EXPECT_FALSE(Graph.verify());
}

TEST(MAIRGraph, FunctionLocalObservationKindsVerify) {
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  auto M = llvm::parseAssemblyString(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  store i32 %v, ptr %p, align 4
  ret void
}
)IR",
                                     Err, Ctx);
  ASSERT_TRUE(M);

  const llvm::Function &F = *M->begin();
  const llvm::BasicBlock &BB = *F.begin();
  const llvm::Instruction &Load = *BB.begin();

  MAIRGraph Graph(F);
  auto &Entry = Graph.createObservation("bb0", &BB, nullptr,
                                        Observation::SiteKind::BasicBlockEntry);
  auto &LoadObs = Graph.createObservation("o0", &BB, &Load,
                                          Observation::SiteKind::MemoryInst);
  LoadObs.addAccess(MemoryAccess::fromInstruction(Load, M->getDataLayout()));
  Graph.addEdge(Entry, LoadObs);

  EXPECT_TRUE(Graph.verify());
  EXPECT_TRUE(Entry.isBasicBlockEntrySite());
  EXPECT_TRUE(LoadObs.isMemoryInstSite());
  ASSERT_EQ(LoadObs.accesses().size(), 1u);
  EXPECT_EQ(LoadObs.accesses().front().getParent(), &LoadObs);
  EXPECT_EQ(Graph.successors(Entry).front(), &LoadObs);
}
