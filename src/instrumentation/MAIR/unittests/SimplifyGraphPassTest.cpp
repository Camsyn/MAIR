#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIRPassTestUtil.h"

using namespace mair;

namespace {

TEST(SimplifyGraphPass, SimplifyGraphContractsStraightLineReads) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %first = load i32, ptr %p, align 4
  %second = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  const size_t Before = Graph.size();
  MAIRGraphAnalysisManager AM;
  EXPECT_FALSE(SimplifyGraphPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), Before - 1);
  EXPECT_EQ(accessCount(Graph, "entry"), 2u);
  const Observation *Kept = nullptr;
  for (const Observation &Obs : Graph)
    if (!Obs.empty())
      Kept = &Obs;
  ASSERT_NE(Kept, nullptr);
  EXPECT_EQ(Kept->size(), 2u);
  EXPECT_TRUE(Graph.verify());
}

TEST(SimplifyGraphPass, LeavesASingleReadUnchanged) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %value = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  const size_t Before = Graph.size();
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(SimplifyGraphPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), Before);
  EXPECT_EQ(accessCount(Graph, "entry"), 1u);
  EXPECT_TRUE(Graph.verify());
}

TEST(SimplifyGraphPass, DoesNotContractAcrossALoopBoundary) {
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
  MAIRGraph Graph = buildGraph(*M->getFunction("f"));
  Observation *Outside = findMutableMemoryObservation(Graph, "entry");
  Observation *Inside = findMutableMemoryObservation(Graph, "loop");
  ASSERT_NE(Outside, nullptr);
  ASSERT_NE(Inside, nullptr);
  Graph.addEdge(*Outside, *Inside, ObsEdgeKind::Dom);
  Graph.addEdge(*Inside, *Outside, ObsEdgeKind::PostDom);
  const size_t Before = Graph.size();
  MAIRGraphAnalysisManager AM;
  EXPECT_TRUE(SimplifyGraphPass().run(Graph, AM).areAllPreserved());
  EXPECT_EQ(Graph.size(), Before);
  EXPECT_EQ(accessCount(Graph, "entry"), 1u);
  EXPECT_EQ(accessCount(Graph, "loop"), 1u);
  EXPECT_TRUE(Graph.verify());
}

} // namespace
