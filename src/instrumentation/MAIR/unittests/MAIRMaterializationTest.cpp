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

MAIRGraph buildMaterializationGraph(Function &F) {
  DominatorTree DT(F);
  PostDominatorTree PDT(F);
  LoopInfo LI(DT);
  return MAIRGraph::build(F, std::move(DT), std::move(PDT), std::move(LI));
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
  unsigned EmittedProgramPoints = 0;
  unsigned EmittedObservations = 0;
  unsigned EmittedMaskedObservations = 0;
  bool SawCompletePlan = true;
  SmallVector<AccessProperty, 4> PlannedProperties;
  SmallVector<AccessProperty, 4> EmittedProperties;

  Error prepare(const MAIRMaterializationPlan &Plan) override {
    PreparedUnits = Plan.units().size();
    for (const MaterializationUnit &Unit : Plan.units()) {
      SawCompletePlan &= Unit.Source && !Unit.Observations.empty();
      SawCompletePlan &=
          Unit.Site.getBasicBlock() == Unit.Site.getAnchor().getParent();
      if (Unit.Site.isEdge())
        SawCompletePlan &= Unit.Site.getEdgeFrom() && Unit.Site.getEdgeTo();
      for (const PlannedObservation &Obs : Unit.Observations) {
        SawCompletePlan &= Obs.Source != nullptr;
        PlannedProperties.push_back(Obs.Properties);
      }
    }
    if (Reject)
      return createStringError(inconvertibleErrorCode(),
                               "recording backend rejected plan");
    return Error::success();
  }

  void emit(const MaterializedProgramPoint &ProgramPoint,
            IRBuilderBase &Builder) override {
    ++EmittedProgramPoints;
    EmittedObservations += ProgramPoint.Observations.size();
    for (const MaterializedObservation &Obs : ProgramPoint.Observations) {
      EmittedProperties.push_back(Obs.Properties);
      Builder.CreateFreeze(Obs.Address, "mair.test.address");
      Builder.CreateFreeze(Obs.AccessSize, "mair.test.size");
      if (Obs.Stride)
        Builder.CreateFreeze(Obs.Stride, "mair.test.stride");
      if (Obs.TripCount)
        Builder.CreateFreeze(Obs.TripCount, "mair.test.trips");
      if (Obs.Mask) {
        ++EmittedMaskedObservations;
        SawCompletePlan &= Obs.MaskElementSize != 0;
        Builder.CreateFreeze(Obs.Mask, "mair.test.mask");
      }
    }
  }
};

TEST(MAIRMaterialization, PreservesObservationProperties) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load volatile i32, ptr %p, align 4
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildMaterializationGraph(F);
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

  ASSERT_EQ(Backend.PlannedProperties.size(), 1u);
  ASSERT_EQ(Backend.EmittedProperties.size(), 1u);
  EXPECT_EQ(Backend.PlannedProperties.front(), AccessProperty::Volatile);
  EXPECT_EQ(Backend.EmittedProperties.front(), AccessProperty::Volatile);
}

TEST(MAIRMaterialization, LowersMaskedAccessSemantics) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, <4 x i1>, <4 x i32>)
declare void @llvm.masked.store.v4i32.p0(<4 x i32>, ptr, <4 x i1>)
define void @f(ptr %p, <4 x i1> %mask, <4 x i32> %value) {
entry:
  %loaded = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %p, <4 x i1> <i1 true, i1 false, i1 true, i1 false>,
      <4 x i32> zeroinitializer)
  call void @llvm.masked.store.v4i32.p0(
      <4 x i32> %value, ptr %p, <4 x i1> %mask)
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildMaterializationGraph(F);
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

  EXPECT_EQ(Backend.EmittedMaskedObservations, 2u);
  EXPECT_TRUE(Backend.SawCompletePlan);
  EXPECT_FALSE(verifyModule(*M, &errs()));
  EXPECT_NE(printModule(*M).find("mair.test.mask"), std::string::npos);
}

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
  MAIRGraph Graph = buildMaterializationGraph(F);
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
  Graph.createInstProgramPoint(Entry, Entry.front());
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
  ProgramPoint &PP = Graph.createEdgeProgramPoint(Entry, Exit);
  PP.addObservation(Observation::fromInstruction(Load, M->getDataLayout()));
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
  ProgramPoint &PP = Graph.createProgramPoint(ProgramPointSite::anchored(
      *Merge->getTerminator(), ProgramPoint::SiteKind::BasicBlockExit));
  MAIRScalarExpr P = Graph.createValueExpr(*F.getArg(0));
  MAIRScalarExpr BadExpr = Graph.createValueExpr(*Bad);
  MAIRScalarExpr One =
      Graph.createConstantExpr(*Type::getInt64Ty(Ctx), APInt(64, 1));
  PP.addObservation(Observation(
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
  ProgramPoint &PP = Graph.createProgramPoint(ProgramPointSite::anchored(
      *Merge->getTerminator(), ProgramPoint::SiteKind::BasicBlockExit));
  PP.addObservation(Observation({},
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
  ProgramPoint &PP = Graph.createEdgeProgramPoint(
      *Header, *Exit, AnyProvenanceExecutedGuard({LoadA, LoadB}));
  PP.addObservation(Observation::fromInstruction(*LoadA, M->getDataLayout()));
  ProgramPoint &Second = Graph.createEdgeProgramPoint(*Header, *Exit);
  Second.addObservation(
      Observation::fromInstruction(*LoadB, M->getDataLayout()));
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
  EXPECT_EQ(Result->LoweredProgramPoints, 2u);
  EXPECT_TRUE(Result->CFGChanged);
  EXPECT_EQ(Backend.PreparedUnits, 2u);
  EXPECT_TRUE(Backend.SawCompletePlan);
  EXPECT_EQ(Backend.EmittedProgramPoints, 2u);
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
  ProgramPoint &PP = Graph.createProgramPoint(ProgramPointSite::anchored(
      Return, ProgramPoint::SiteKind::BasicBlockExit));
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
  PP.addObservation(Observation({}, MemoryRegion(Region), AccessKind::Read));
  PP.addObservation(Observation({}, MemoryRegion(Region), AccessKind::Write));
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

  unsigned NumGEPs = 0;
  unsigned NumSMaxCmps = 0;
  unsigned NumUMaxCmps = 0;
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      NumGEPs += I.getName().starts_with("mair.expr.gep");
      NumSMaxCmps += I.getName().starts_with("mair.expr.smax.cmp");
      NumUMaxCmps += I.getName().starts_with("mair.expr.umax.cmp");
    }
  EXPECT_EQ(NumGEPs, 1u);
  EXPECT_EQ(NumSMaxCmps, 1u);
  EXPECT_EQ(NumUMaxCmps, 1u);
}

TEST(MAIRMaterialization, RetargetsMultipleProgramPointsAtOneAnchor) {
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
  ProgramPointSite Site = ProgramPointSite::anchored(
      Return, ProgramPoint::SiteKind::BasicBlockExit);

  MAIRGraph Graph(F);
  ProgramPoint &Guarded =
      Graph.createProgramPoint(Site, AnyProvenanceExecutedGuard({&Load}));
  Guarded.addObservation(
      Observation::fromInstruction(Load, M->getDataLayout()));
  ProgramPoint &Always = Graph.createProgramPoint(Site);
  Always.addObservation(Observation::fromInstruction(Load, M->getDataLayout()));
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
  EXPECT_EQ(Result->GuardBranches, 0u);
  EXPECT_EQ(Result->LoweredProgramPoints, 2u);
  EXPECT_FALSE(Result->CFGChanged);
  EXPECT_EQ(Backend.EmittedProgramPoints, 2u);
  EXPECT_TRUE(Graph.isConsumed());
  EXPECT_FALSE(verifyModule(*M, &errs()));
  std::string Text = printModule(*M);
  EXPECT_EQ(Text.find("br i1"), std::string::npos);
  EXPECT_EQ(Text.find("poison"), std::string::npos);
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
  LoopInfo LI(DT);
  MAIRGraph Graph =
      MAIRGraph::build(F, std::move(DT), std::move(PDT), std::move(LI));

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
  EXPECT_EQ(Result->LoweredProgramPoints, 1u);
  EXPECT_FALSE(Result->CFGChanged);
  EXPECT_EQ(FAM.getCachedResult<DominatorTreeAnalysis>(F), nullptr);
  EXPECT_TRUE(Graph.isConsumed());
  EXPECT_FALSE(Graph.verify());

  auto Second = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Second);
  EXPECT_NE(toString(Second.takeError()).find("already consumed"),
            std::string::npos);
}

TEST(MAIRMaterialization, NestedGuardResetsAtEachInnerLoopInvocation) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p) {
entry:
  br label %outer
outer:
  %oi = phi i32 [ 0, %entry ], [ %onext, %outer.latch ]
  br label %inner.preheader
inner.preheader:
  br label %inner
inner:
  %ii = phi i32 [ 0, %inner.preheader ], [ %inext, %inner.latch ]
  %more = icmp ult i32 %ii, 2
  br i1 %more, label %dispatch, label %inner.exit
dispatch:
  %first = icmp eq i32 %oi, 0
  br i1 %first, label %access, label %inner.latch
access:
  %value = load i32, ptr %p, align 4
  br label %inner.latch
inner.latch:
  %inext = add i32 %ii, 1
  br label %inner
inner.exit:
  br label %outer.latch
outer.latch:
  %onext = add i32 %oi, 1
  %done = icmp eq i32 %onext, 2
  br i1 %done, label %exit, label %outer
exit:
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  BasicBlock *Reset = nullptr;
  BasicBlock *Target = nullptr;
  Instruction *Load = nullptr;
  for (BasicBlock &Block : F) {
    if (Block.getName() == "inner.preheader")
      Reset = &Block;
    if (Block.getName() == "inner.exit")
      Target = &Block;
    if (Block.getName() == "access")
      Load = &Block.front();
  }
  ASSERT_TRUE(Reset && Target && Load);
  MAIRGraph Graph = buildMaterializationGraph(F);
  for (ProgramPoint &Existing : Graph)
    while (!Existing.empty())
      Existing.erase(Existing.begin());
  ProgramPoint &Point = Graph.createProgramPoint(
      ProgramPointSite::anchored(Target->front(),
                                 ProgramPoint::SiteKind::BasicBlockEntry),
      AnyProvenanceExecutedGuard({Load}, Reset));
  Point.addObservation(Observation::fromInstruction(*Load, M->getDataLayout()));
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
  EXPECT_EQ(Result->GuardBranches, 1u);
  bool SawResetIncoming = false;
  for (const BasicBlock &Block : F)
    for (const PHINode &Phi : Block.phis()) {
      if (!Phi.getName().starts_with("mair.executed"))
        continue;
      int Index = Phi.getBasicBlockIndex(Reset);
      if (Index < 0)
        continue;
      const auto *Initial = dyn_cast<ConstantInt>(Phi.getIncomingValue(Index));
      ASSERT_NE(Initial, nullptr);
      EXPECT_TRUE(Initial->isZero());
      SawResetIncoming = true;
    }
  EXPECT_TRUE(SawResetIncoming);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(MAIRMaterialization, ExplicitEntryResetHasOneInitialSSAValue) {
  LLVMContext Ctx;
  auto M = parseMaterializationModule(R"IR(
define void @f(ptr %p, i1 %take) {
entry:
  br i1 %take, label %access, label %exit
access:
  %value = load i32, ptr %p, align 4
  br label %exit
exit:
  ret void
}
)IR",
                                      Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  BasicBlock &Access = *std::next(F.begin());
  BasicBlock &Exit = *std::next(F.begin(), 2);
  Instruction &Load = Access.front();
  MAIRGraph Graph = buildMaterializationGraph(F);
  for (ProgramPoint &Existing : Graph)
    while (!Existing.empty())
      Existing.erase(Existing.begin());
  ProgramPoint &Point = Graph.createProgramPoint(
      ProgramPointSite::anchored(Exit.front(),
                                 ProgramPoint::SiteKind::BasicBlockEntry),
      AnyProvenanceExecutedGuard({&Load}, &F.getEntryBlock()));
  Point.addObservation(Observation::fromInstruction(Load, M->getDataLayout()));
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
  EXPECT_EQ(Result->GuardBranches, 1u);
  EXPECT_EQ(Backend.EmittedObservations, 1u);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

} // namespace
