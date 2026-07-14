#include "MAIR/Materialization/MAIRMaterialization.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/Analysis/PostDominators.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace mair;

namespace {

std::unique_ptr<Module> parseMaterializationModule(StringRef IR,
                                                   LLVMContext &Ctx) {
  SMDiagnostic Err;
  return parseAssemblyString(IR, Err, Ctx);
}

std::string printModule(const Module &M) {
  std::string Text;
  raw_string_ostream OS(Text);
  M.print(OS, nullptr);
  return OS.str();
}

void registerMaterializationAnalyses(FunctionAnalysisManager &FAM,
                                     PassInstrumentationCallbacks &PIC) {
  FAM.registerPass([&] { return PassInstrumentationAnalysis(&PIC); });
  FAM.registerPass([] { return DominatorTreeAnalysis(); });
  FAM.registerPass([] { return PostDominatorTreeAnalysis(); });
  FAM.registerPass([] { return llvm::LoopAnalysis(); });
}

class RecordingBackend final : public MAIRObservationBackend {
public:
  bool Reject = false;
  unsigned PreparedUnits = 0;
  unsigned EmittedObservations = 0;
  unsigned EmittedAccesses = 0;
  bool SawCompletePlan = true;

  Error prepare(const MAIRMaterializationPlan &Plan) override {
    PreparedUnits = Plan.units().size();
    for (const MaterializationUnit &Unit : Plan.units()) {
      SawCompletePlan &= Unit.Source && !Unit.Accesses.empty();
      SawCompletePlan &=
          Unit.Site.isEdge()
              ? Unit.Site.getEdgeFrom() && Unit.Site.getEdgeTo()
              : Unit.Site.getBasicBlock() && Unit.Site.getAnchor();
      for (const PlannedAccess &Access : Unit.Accesses)
        SawCompletePlan &= Access.Source != nullptr;
    }
    if (Reject)
      return createStringError(inconvertibleErrorCode(),
                               "recording backend rejected plan");
    return Error::success();
  }

  void emit(const MaterializedObservation &Observation,
            IRBuilderBase &Builder) override {
    ++EmittedObservations;
    EmittedAccesses += Observation.Accesses.size();
    for (const MaterializedAccess &Access : Observation.Accesses) {
      Builder.CreateFreeze(Access.Address, "mair.test.address");
      Builder.CreateFreeze(Access.AccessSize, "mair.test.size");
      if (Access.Stride)
        Builder.CreateFreeze(Access.Stride, "mair.test.stride");
      if (Access.TripCount)
        Builder.CreateFreeze(Access.TripCount, "mair.test.trips");
    }
  }
};

TEST(MAIRMaterialization, BackendRejectionLeavesModuleAndGraphLive) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = MAIRGraph::build(F, DominatorTree(F), PostDominatorTree(F));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  Backend.Reject = true;
  std::string Before = printModule(*M);

  auto Result = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Result);
  EXPECT_NE(toString(Result.takeError()).find("recording backend rejected"),
            std::string::npos);
  EXPECT_EQ(printModule(*M), Before);
  EXPECT_TRUE(Graph.isLive());
  EXPECT_TRUE(Graph.verify());
}

TEST(MAIRMaterialization, RequiresExplicitMutableFunctionOwnership) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  const Function &F = *M->getFunction("f");
  MAIRGraph Graph(F);
  const BasicBlock &Entry = F.getEntryBlock();
  Graph.createInstObservation(Entry, Entry.front());
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  std::string Before = printModule(*M);
  auto Result = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Result);
  EXPECT_NE(toString(Result.takeError()).find("mutable Function"),
            std::string::npos);
  EXPECT_EQ(printModule(*M), Before);
  EXPECT_EQ(Backend.PreparedUnits, 0u);
  EXPECT_TRUE(Graph.isLive());
}

TEST(MAIRMaterialization, RejectsNonCriticalEdgeBeforeMutation) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
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
  Function &F = *M->getFunction("f");
  BasicBlock &Entry = F.getEntryBlock();
  BasicBlock &Exit = F.back();
  Instruction &Load = Entry.front();
  MAIRGraph Graph(F);
  Observation &Obs = Graph.createEdgeObservation(Entry, Exit);
  Obs.addAccess(MemoryAccess::fromInstruction(Load, M->getDataLayout()));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  std::string Before = printModule(*M);

  auto Result = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Result);
  EXPECT_NE(toString(Result.takeError()).find("unsupported edge site"),
            std::string::npos);
  EXPECT_EQ(printModule(*M), Before);
  EXPECT_EQ(Backend.PreparedUnits, 0u);
  EXPECT_TRUE(Graph.isLive());
}

TEST(MAIRMaterialization, RejectsUnavailableScalarLeafBeforeMutation) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p, i64 %n, i1 %take) {
entry:
  br i1 %take, label %left, label %right
left:
  %bad = add i64 %n, 1
  br label %merge
right:
  br label %merge
merge:
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  BasicBlock *Left = nullptr;
  BasicBlock *Merge = nullptr;
  Instruction *Bad = nullptr;
  for (BasicBlock &BB : F) {
    if (BB.getName() == "left") {
      Left = &BB;
      Bad = &BB.front();
    } else if (BB.getName() == "merge") {
      Merge = &BB;
    }
  }
  ASSERT_TRUE(Left && Merge && Bad);

  MAIRGraph Graph(F);
  Observation &Obs = Graph.createObservation(ObservationSite::anchored(
      Merge, Merge->getTerminator(), Observation::SiteKind::BasicBlockExit));
  MAIRScalarExpr P = Graph.createValueExpr(*F.getArg(0));
  MAIRScalarExpr BadExpr = Graph.createValueExpr(*Bad);
  MAIRScalarExpr One =
      Graph.createConstantExpr(*Type::getInt64Ty(Ctx), APInt(64, 1));
  Obs.addAccess(MemoryAccess(
      {}, PeriodicRegion(F.getArg(0), P, BadExpr, One, One), AccessKind::Read));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  std::string Before = printModule(*M);
  auto Result = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Result);
  EXPECT_NE(toString(Result.takeError()).find("unavailable"),
            std::string::npos);
  EXPECT_EQ(printModule(*M), Before);
  EXPECT_EQ(Backend.PreparedUnits, 0u);
  EXPECT_TRUE(Graph.isLive());
}

TEST(MAIRMaterialization, RejectsUnavailableContiguousBaseBeforeMutation) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(i64 %address, i1 %take) {
entry:
  br i1 %take, label %left, label %right
left:
  %bad = inttoptr i64 %address to ptr
  br label %merge
right:
  br label %merge
merge:
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  Instruction *Bad = nullptr;
  BasicBlock *Merge = nullptr;
  for (BasicBlock &BB : F) {
    if (BB.getName() == "left")
      Bad = &BB.front();
    else if (BB.getName() == "merge")
      Merge = &BB;
  }
  ASSERT_TRUE(Bad && Merge);

  MAIRGraph Graph(F);
  Observation &Obs = Graph.createObservation(ObservationSite::anchored(
      Merge, Merge->getTerminator(), Observation::SiteKind::BasicBlockExit));
  Obs.addAccess(MemoryAccess({},
                             AbstractAccess(Bad, AbstractValue::constant(0),
                                            AbstractValue::constant(4)),
                             AccessKind::Read));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  std::string Before = printModule(*M);
  auto Result = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Result);
  EXPECT_NE(toString(Result.takeError()).find("unavailable"),
            std::string::npos);
  EXPECT_EQ(printModule(*M), Before);
  EXPECT_EQ(Backend.PreparedUnits, 0u);
  EXPECT_TRUE(Graph.isLive());
}

TEST(MAIRMaterialization, SplitsCriticalExitAndBuildsAnyProvenanceGuard) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p, ptr %q, i64 %n, i1 %take_a, i1 %take_b, i1 %bypass) {
entry:
  br i1 %bypass, label %other, label %preheader
preheader:
  br label %header
header:
  %i = phi i64 [ 0, %preheader ], [ %next, %latch ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %body, label %exit
body:
  br i1 %take_a, label %access.a, label %check.b
access.a:
  %a = load i8, ptr %p, align 1
  br label %latch
check.b:
  br i1 %take_b, label %access.b, label %latch
access.b:
  %b = load i8, ptr %q, align 1
  br label %latch
latch:
  %next = add i64 %i, 1
  br label %header
other:
  br label %exit
exit:
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  BasicBlock *Header = nullptr;
  BasicBlock *Exit = nullptr;
  Instruction *LoadA = nullptr;
  Instruction *LoadB = nullptr;
  for (BasicBlock &BB : F) {
    if (BB.getName() == "header")
      Header = &BB;
    else if (BB.getName() == "exit")
      Exit = &BB;
    else if (BB.getName() == "access.a")
      LoadA = &BB.front();
    else if (BB.getName() == "access.b")
      LoadB = &BB.front();
  }
  ASSERT_TRUE(Header && Exit && LoadA && LoadB);

  MAIRGraph Graph(F);
  Observation &Obs = Graph.createEdgeObservation(
      *Header, *Exit, AnyProvenanceExecutedGuard({LoadA, LoadB}));
  Obs.addAccess(MemoryAccess::fromInstruction(*LoadA, M->getDataLayout()));
  Observation &Second = Graph.createEdgeObservation(*Header, *Exit);
  Second.addAccess(MemoryAccess::fromInstruction(*LoadB, M->getDataLayout()));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }

  EXPECT_EQ(Result->SplitEdges, 1u);
  EXPECT_EQ(Result->GuardBranches, 1u);
  EXPECT_EQ(Result->LoweredObservations, 2u);
  EXPECT_TRUE(Result->CFGChanged);
  EXPECT_EQ(Backend.PreparedUnits, 2u);
  EXPECT_TRUE(Backend.SawCompletePlan);
  EXPECT_EQ(Backend.EmittedObservations, 2u);
  EXPECT_TRUE(Graph.isConsumed());
  EXPECT_FALSE(verifyModule(*M, &errs()));

  std::string Text = printModule(*M);
  EXPECT_NE(Text.find("mair.edge"), std::string::npos);
  EXPECT_NE(Text.find("mair.executed"), std::string::npos);
  EXPECT_NE(Text.find("phi i1 [ false"), std::string::npos);
  EXPECT_NE(Text.find("br i1 %mair.executed"), std::string::npos);
}

TEST(MAIRMaterialization, LowersCompletePeriodicExpressionDag) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p, i32 %n, i64 %denom) {
entry:
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  BasicBlock &Entry = F.getEntryBlock();
  Instruction &Return = *Entry.getTerminator();
  Argument &P = *F.getArg(0);
  Argument &N = *F.getArg(1);
  Argument &Denom = *F.getArg(2);
  Type &I32 = *Type::getInt32Ty(Ctx);
  Type &I64 = *Type::getInt64Ty(Ctx);

  MAIRGraph Graph(F);
  Observation &Obs = Graph.createObservation(ObservationSite::anchored(
      &Entry, &Return, Observation::SiteKind::BasicBlockExit));
  MAIRScalarExpr PExpr = Graph.createValueExpr(P);
  MAIRScalarExpr NExpr = Graph.createValueExpr(N);
  MAIRScalarExpr DenomExpr = Graph.createValueExpr(Denom);
  MAIRScalarExpr C2 = Graph.createConstantExpr(I64, APInt(64, 2));
  MAIRScalarExpr C3 = Graph.createConstantExpr(I64, APInt(64, 3));
  MAIRScalarExpr C8 = Graph.createConstantExpr(I64, APInt(64, 8));
  MAIRScalarExpr WideN =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::ZeroExtend, I64, NExpr);
  MAIRScalarExpr Pointer = Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add,
                                                  *P.getType(), PExpr, WideN);
  MAIRScalarExpr Mul =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::Mul, I64, C3, WideN);
  MAIRScalarExpr Div =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::UDiv, I64, Mul, DenomExpr);
  MAIRScalarExpr SMin =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::SMin, I64, Div, C8);
  MAIRScalarExpr UMin =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::UMin, I64, SMin, C8);
  MAIRScalarExpr SMax =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::SMax, I64, UMin, C2);
  MAIRScalarExpr UMax =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::UMax, I64, SMax, C3);
  MAIRScalarExpr Trunc =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::Truncate, I32, UMax);
  MAIRScalarExpr Stride =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::SignExtend, I64, Trunc);
  PeriodicRegion Region(&P, Pointer, Stride, C2, WideN);
  Obs.addAccess(MemoryAccess({}, MemoryRegion(Region), AccessKind::Read));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }
  EXPECT_FALSE(verifyModule(*M, &errs()));
  EXPECT_TRUE(Backend.SawCompletePlan);

  std::string Text = printModule(*M);
  EXPECT_NE(Text.find("mair.expr.gep"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.mul"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.udiv"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.zext"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.trunc"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.sext"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.smin"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.umin"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.smax"), std::string::npos);
  EXPECT_NE(Text.find("mair.expr.umax"), std::string::npos);
}

TEST(MAIRMaterialization, RetargetsMultipleObservationsAtOneAnchor) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  BasicBlock &Entry = F.getEntryBlock();
  Instruction &Load = Entry.front();
  Instruction &Return = *Entry.getTerminator();
  ObservationSite Site = ObservationSite::anchored(
      &Entry, &Return, Observation::SiteKind::BasicBlockExit);

  MAIRGraph Graph(F);
  Observation &Guarded =
      Graph.createObservation(Site, AnyProvenanceExecutedGuard({&Load}));
  Guarded.addAccess(MemoryAccess::fromInstruction(Load, M->getDataLayout()));
  Observation &Always = Graph.createObservation(Site);
  Always.addAccess(MemoryAccess::fromInstruction(Load, M->getDataLayout()));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }

  EXPECT_EQ(Result->SplitEdges, 0u);
  EXPECT_EQ(Result->GuardBranches, 1u);
  EXPECT_EQ(Result->LoweredObservations, 2u);
  EXPECT_TRUE(Result->CFGChanged);
  EXPECT_EQ(Backend.EmittedObservations, 2u);
  EXPECT_TRUE(Graph.isConsumed());
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(MAIRMaterialization, InvalidatesFunctionAnalysesAndIsSingleUse) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  DominatorTree DT(F);
  PostDominatorTree PDT(F);
  MAIRGraph Graph = MAIRGraph::build(F, DT, PDT);

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  (void)FAM.getResult<DominatorTreeAnalysis>(F);
  ASSERT_NE(FAM.getCachedResult<DominatorTreeAnalysis>(F), nullptr);

  RecordingBackend Backend;
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }
  EXPECT_EQ(Result->SplitEdges, 0u);
  EXPECT_EQ(Result->GuardBranches, 0u);
  EXPECT_EQ(Result->LoweredObservations, 1u);
  EXPECT_FALSE(Result->CFGChanged);
  EXPECT_EQ(FAM.getCachedResult<DominatorTreeAnalysis>(F), nullptr);
  EXPECT_TRUE(Graph.isConsumed());
  EXPECT_FALSE(Graph.verify());

  auto Second = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Second);
  EXPECT_NE(toString(Second.takeError()).find("already consumed"),
            std::string::npos);
}

} // namespace
