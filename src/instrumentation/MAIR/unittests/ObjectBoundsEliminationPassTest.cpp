#include "MAIR/Transforms/ObjectBoundsEliminationPass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIRPassTestUtil.h"

#include "llvm/IR/GlobalVariable.h"

using namespace mair;

namespace {

// @T is a 256-byte table the test oracle trusts, @M one it does not.
constexpr llvm::StringLiteral Tables = R"IR(
@T = internal constant [256 x i8] zeroinitializer
@M = global [256 x i8] zeroinitializer
)IR";

/// The ASan configuration with an oracle that trusts @T only.
MAIROptimizationConfig trustingT() {
  MAIROptimizationConfig Config = MAIROptimizationConfig::forAddressSanitizer();
  Config.setTrustedObjectSize(
      [](const llvm::Value &Base) -> std::optional<uint64_t> {
        if (Base.getName() == "T")
          return 256;
        return std::nullopt;
      });
  return Config;
}

struct Fixture {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::FunctionAnalysisManager FAM;
  MAIRGraphAnalysisManager AM;

  explicit Fixture(llvm::StringRef Body) {
    M = parseModule((Tables + Body).str(), Ctx);
    EXPECT_TRUE(M);
    registerLoopAnalyses(FAM, PIC);
    registerMAIRGraphAnalyses(AM, &PIC, &FAM);
  }

  MAIRGraph build(MAIROptimizationConfig Config = trustingT()) {
    return createGraphWithFunctionAnalyses(*M->getFunction("f"), FAM, Config);
  }
};

unsigned totalObservations(const MAIRGraph &Graph) {
  unsigned Count = 0;
  for (const ProgramPoint *PP : Graph.allProgramPoints())
    Count += PP->size();
  return Count;
}

constexpr llvm::StringLiteral ByteIndex = R"IR(
define i8 @f(i8 %c) {
entry:
  %i = zext i8 %c to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %v = load i8, ptr %p, align 1
  ret i8 %v
}
)IR";

TEST(ObjectBoundsEliminationPass, EliminatesAByteIndexIntoATrustedTable) {
  Fixture Test(ByteIndex);
  MAIRGraph Graph = Test.build();
  ASSERT_EQ(totalObservations(Graph), 1u);
  EXPECT_FALSE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                   .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 0u);
  EXPECT_TRUE(Graph.verify());
}

TEST(ObjectBoundsEliminationPass, KeepsEverythingWithoutAnOracle) {
  Fixture Test(ByteIndex);
  MAIRGraph Graph = Test.build(MAIROptimizationConfig::forAddressSanitizer());
  EXPECT_TRUE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                  .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 1u);
}

TEST(ObjectBoundsEliminationPass, KeepsAnIndexThatMayReachTheEnd) {
  Fixture Test(R"IR(
define i8 @f(i8 %c) {
entry:
  %i = zext i8 %c to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %q = getelementptr inbounds i8, ptr %p, i64 1
  %v = load i8, ptr %q, align 1
  ret i8 %v
}
)IR");
  MAIRGraph Graph = Test.build();
  EXPECT_TRUE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                  .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 1u);
}

TEST(ObjectBoundsEliminationPass, KeepsAnAccessThatMayStraddleTheEnd) {
  // Bytes 255 and 256 for c == 255.
  Fixture Test(R"IR(
define i16 @f(i8 %c) {
entry:
  %i = zext i8 %c to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %v = load i16, ptr %p, align 1
  ret i16 %v
}
)IR");
  MAIRGraph Graph = Test.build();
  EXPECT_TRUE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                  .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 1u);
}

TEST(ObjectBoundsEliminationPass, KeepsAnUntrustedObject) {
  Fixture Test(R"IR(
define i8 @f(i8 %c) {
entry:
  %i = zext i8 %c to i64
  %p = getelementptr inbounds i8, ptr @M, i64 %i
  %v = load i8, ptr %p, align 1
  ret i8 %v
}
)IR");
  MAIRGraph Graph = Test.build();
  EXPECT_TRUE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                  .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 1u);
}

TEST(ObjectBoundsEliminationPass, KeepsANegativeIndex) {
  // %i is in [-128, 127]: below the start for negative c.
  Fixture Test(R"IR(
define i8 @f(i8 %c) {
entry:
  %i = sext i8 %c to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %v = load i8, ptr %p, align 1
  ret i8 %v
}
)IR");
  MAIRGraph Graph = Test.build();
  EXPECT_TRUE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                  .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 1u);
}

TEST(ObjectBoundsEliminationPass, EliminatesAnIndexBoundedByADominatingTest) {
  // Only the branch condition bounds %idx: 50 <= %idx + 50 < 250.
  Fixture Test(R"IR(
define i8 @f(i32 %idx) {
entry:
  %in = icmp ult i32 %idx, 200
  br i1 %in, label %then, label %else
then:
  %i = zext i32 %idx to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %q = getelementptr inbounds i8, ptr %p, i64 50
  %v = load i8, ptr %q, align 1
  ret i8 %v
else:
  ret i8 0
}
)IR");
  MAIRGraph Graph = Test.build();
  EXPECT_FALSE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                   .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 0u);
}

TEST(ObjectBoundsEliminationPass, KeepsAnIndexBoundedOnlyOnAnotherPath) {
  // The test bounds %idx in %then, but the access is in %join.
  Fixture Test(R"IR(
define i8 @f(i32 %idx) {
entry:
  %in = icmp ult i32 %idx, 200
  br i1 %in, label %then, label %join
then:
  call void @g()
  br label %join
join:
  %i = zext i32 %idx to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %v = load i8, ptr %p, align 1
  ret i8 %v
}
declare void @g()
)IR");
  MAIRGraph Graph = Test.build();
  EXPECT_TRUE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                  .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 1u);
}

TEST(ObjectBoundsEliminationPass, FollowsTheDeletabilityOfAVolatileAccess) {
  // ASan deletes volatile observations like any other.
  Fixture Test(R"IR(
define i8 @f(i8 %c) {
entry:
  %i = zext i8 %c to i64
  %p = getelementptr inbounds i8, ptr @T, i64 %i
  %v = load volatile i8, ptr %p, align 1
  ret i8 %v
}
)IR");
  MAIRGraph Graph = Test.build();
  EXPECT_FALSE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM)
                   .areAllPreserved());
  EXPECT_EQ(totalObservations(Graph), 0u);

  // A configuration that keeps volatile observations keeps this one.
  MAIROptimizationConfig Distinguishing =
      MAIROptimizationConfig::forThreadSanitizer(/*DistinguishVolatile=*/true);
  Distinguishing.setTrustedObjectSize(
      [](const llvm::Value &) -> std::optional<uint64_t> { return 256; });
  MAIRGraph Kept = Test.build(Distinguishing);
  EXPECT_TRUE(runPassOnGraphTree(ObjectBoundsEliminationPass(), Kept, Test.AM)
                  .areAllPreserved());
  EXPECT_EQ(totalObservations(Kept), 1u);
}

// A loop over T[0, n) with 0 < n <= LIMIT on entry: periodic combination
// turns the loads into one range [0, n) at the exit.
constexpr llvm::StringLiteral BoundedLoop = R"IR(
define void @f(i64 %n) {
entry:
  %small = icmp ule i64 %n, LIMIT
  %nonzero = icmp ne i64 %n, 0
  %enter = and i1 %small, %nonzero
  br i1 %enter, label %loop, label %done
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %off = mul nuw nsw i64 %i, STRIDE
  %p = getelementptr inbounds i8, ptr @T, i64 %off
  %v = load i8, ptr %p, align 1
  %next = add nuw nsw i64 %i, 1
  %more = icmp ult i64 %next, %n
  br i1 %more, label %loop, label %exit
exit:
  br label %done
done:
  ret void
}
)IR";

enum class Shape { Range, Periodic };

/// The observations left after periodic combination and object bounds
/// elimination, for the loop with \p Limit and \p Stride.
unsigned afterCombination(llvm::StringRef Limit, llvm::StringRef Stride,
                          Shape Expected) {
  std::string Body = BoundedLoop.str();
  Body.replace(Body.find("LIMIT"), 5, Limit.str());
  Body.replace(Body.find("STRIDE"), 6, Stride.str());
  Fixture Test(Body);
  MAIRGraph Graph = Test.build();
  runPassOnGraphTree(LoopPeriodicObservationCombinePass(), Graph, Test.AM);
  unsigned Combined = 0;
  for (const ProgramPoint *PP : Graph.allProgramPoints())
    for (const Observation &Obs : *PP) {
      const AbstractAccess *Region = Obs.getContiguousRegion();
      Combined += Expected == Shape::Range ? Region && Region->hasSizeExpr()
                                           : Obs.isPeriodic();
    }
  EXPECT_EQ(Combined, 1u);
  EXPECT_EQ(totalObservations(Graph), 1u);
  runPassOnGraphTree(ObjectBoundsEliminationPass(), Graph, Test.AM);
  EXPECT_TRUE(Graph.verify());
  return totalObservations(Graph);
}

TEST(ObjectBoundsEliminationPass, EliminatesARangeWithinTheTable) {
  EXPECT_EQ(afterCombination("256", "1", Shape::Range), 0u);
}

TEST(ObjectBoundsEliminationPass, KeepsARangeThatMayPassTheEnd) {
  EXPECT_EQ(afterCombination("257", "1", Shape::Range), 1u);
}

TEST(ObjectBoundsEliminationPass, EliminatesAPeriodicRegionWithinTheTable) {
  // The last trip reads byte 2 * 127 = 254.
  EXPECT_EQ(afterCombination("128", "2", Shape::Periodic), 0u);
}

TEST(ObjectBoundsEliminationPass, KeepsAPeriodicRegionThatMayPassTheEnd) {
  // The last trip may read byte 2 * 128 = 256.
  EXPECT_EQ(afterCombination("129", "2", Shape::Periodic), 1u);
}

} // namespace
