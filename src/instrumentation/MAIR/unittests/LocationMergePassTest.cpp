#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIRPassTestUtil.h"

using namespace mair;

namespace {

TEST(LocationMergePass, LocationMergeJoinsAdjacentReadsAndKeepsKindSplit) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %first = load i32, ptr %p, align 4
  %next = getelementptr i32, ptr %p, i64 1
  %second = load i32, ptr %next, align 4
  store i32 %first, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  Observation *First = findMutableMemoryObservation(Graph, "entry");
  ASSERT_NE(First, nullptr);
  SmallVector<Observation *, 4> Sources;
  for (Observation &Obs : Graph)
    if (Obs.isMemoryInstSite() && &Obs != First)
      Sources.push_back(&Obs);
  ASSERT_EQ(Sources.size(), 2u);
  Observation *Read = nullptr;
  Observation *Write = nullptr;
  for (Observation *Source : Sources) {
    ASSERT_EQ(Source->size(), 1u);
    if (Source->begin()->getKind() == AccessKind::Read)
      Read = Source;
    else
      Write = Source;
  }
  ASSERT_NE(Read, nullptr);
  ASSERT_NE(Write, nullptr);
  First->addAccess(*Read->begin());
  Read->erase(Read->begin());

  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(LocationMergePass().run(Graph, AM).areAllPreserved());
  ASSERT_EQ(First->size(), 1u);
  const AbstractAccess *Merged = First->begin()->getContiguousRegion();
  ASSERT_NE(Merged, nullptr);
  EXPECT_TRUE(Merged->getSize().isConstant());
  EXPECT_EQ(Merged->getSize().getConstant(), 8u);
  EXPECT_EQ(Write->size(), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(LocationMergePass, DoesNotMergeReadAndWriteAtDifferentLevels) {
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
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  Observation *Read = findMutableMemoryObservation(Graph, "entry");
  ASSERT_NE(Read, nullptr);
  Observation *Write = nullptr;
  for (Observation &Obs : Graph)
    if (Obs.isMemoryInstSite() && &Obs != Read)
      Write = &Obs;
  ASSERT_NE(Write, nullptr);
  ASSERT_EQ(Write->begin()->getKind(), AccessKind::Write);
  Read->addAccess(*Write->begin());
  Write->erase(Write->begin());

  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(LocationMergePass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Read->size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(LocationMergePass, DoesNotMergeNonAdjacentReads) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %first = load i32, ptr %p, align 4
  %next = getelementptr i32, ptr %p, i64 2
  %second = load i32, ptr %next, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  Observation *First = findMutableMemoryObservation(Graph, "entry");
  ASSERT_NE(First, nullptr);
  Observation *Second = nullptr;
  for (Observation &Obs : Graph)
    if (Obs.isMemoryInstSite() && &Obs != First)
      Second = &Obs;
  ASSERT_NE(Second, nullptr);
  First->addAccess(*Second->begin());
  Second->erase(Second->begin());

  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(LocationMergePass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(First->size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
