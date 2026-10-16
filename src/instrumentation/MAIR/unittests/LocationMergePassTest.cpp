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
  ProgramPoint *First = findMutableMemoryProgramPoint(Graph, "entry");
  ASSERT_NE(First, nullptr);
  SmallVector<ProgramPoint *, 4> Sources;
  for (ProgramPoint &PP : Graph)
    if (PP.isMemoryInstSite() && &PP != First)
      Sources.push_back(&PP);
  ASSERT_EQ(Sources.size(), 2u);
  ProgramPoint *Read = nullptr;
  ProgramPoint *Write = nullptr;
  for (ProgramPoint *Source : Sources) {
    ASSERT_EQ(Source->size(), 1u);
    if (Source->begin()->getKind() == AccessKind::Read)
      Read = Source;
    else
      Write = Source;
  }
  ASSERT_NE(Read, nullptr);
  ASSERT_NE(Write, nullptr);
  First->addObservation(*Read->begin());
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
  ProgramPoint *Read = findMutableMemoryProgramPoint(Graph, "entry");
  ASSERT_NE(Read, nullptr);
  ProgramPoint *Write = nullptr;
  for (ProgramPoint &PP : Graph)
    if (PP.isMemoryInstSite() && &PP != Read)
      Write = &PP;
  ASSERT_NE(Write, nullptr);
  ASSERT_EQ(Write->begin()->getKind(), AccessKind::Write);
  Read->addObservation(*Write->begin());
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
  ProgramPoint *First = findMutableMemoryProgramPoint(Graph, "entry");
  ASSERT_NE(First, nullptr);
  ProgramPoint *Second = nullptr;
  for (ProgramPoint &PP : Graph)
    if (PP.isMemoryInstSite() && &PP != First)
      Second = &PP;
  ASSERT_NE(Second, nullptr);
  First->addObservation(*Second->begin());
  Second->erase(Second->begin());

  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(LocationMergePass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(First->size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
