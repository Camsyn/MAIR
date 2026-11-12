#include "MAIR/Materialization/MAIRMaterialization.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/Transforms/LoopPeriodicObservationCombinePass.h"
#include "MAIRHierarchyTestUtil.h"
#include "MAIRPassTestUtil.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace mair;

namespace {

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
  unsigned PrepareCalls = 0;
  unsigned PreparedUnits = 0;
  unsigned EmittedProgramPoints = 0;
  unsigned EmittedObservations = 0;
  unsigned EmittedMaskedObservations = 0;
  bool SawCompletePlan = true;
  SmallVector<AccessProperty, 4> PlannedProperties;
  SmallVector<AccessProperty, 4> EmittedProperties;

  Error prepare(const MAIRMaterializationPlan &Plan) override {
    ++PrepareCalls;
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
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load volatile i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
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
  auto M = parseModule(R"IR(
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
  MAIRGraph Graph = buildGraph(F);
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
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
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
  auto M = parseModule(R"IR(
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
  auto M = parseModule(R"IR(
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

TEST(MAIRMaterialization, RejectsPointerContiguousSizeBeforeMutation) {
  LLVMContext Ctx;
  auto Module = parseModule(R"IR(
define void @f(ptr %base) {
entry:
  ret void
}
)IR",
                            Ctx);
  ASSERT_TRUE(Module);
  Function &Func = *Module->getFunction("f");
  Argument &Base = *Func.getArg(0);
  MAIRGraph Graph(Func);
  ProgramPoint &Point = Graph.createProgramPoint(
      ProgramPointSite::anchored(*Func.getEntryBlock().getTerminator(),
                                 ProgramPoint::SiteKind::BasicBlockExit));
  MAIRScalarExpr BaseExpr = Graph.createValueExpr(Base);
  MAIRScalarExpr One =
      Graph.createConstantExpr(*Type::getInt64Ty(Ctx), APInt(64, 1));
  MAIRScalarExpr SizeExpr = Graph.createBinaryExpr(
      MAIRScalarExpr::Kind::Add, *Base.getType(), BaseExpr, One);
  AbstractAccess Region(&Base, AbstractValue::constant(0), AbstractValue());
  Region.setSizeExpr(SizeExpr);
  Point.addObservation(Observation({}, Region, AccessKind::Read));

  std::string Diagnostics;
  raw_string_ostream DiagnosticStream(Diagnostics);
  EXPECT_FALSE(Graph.verify(&DiagnosticStream));
  EXPECT_NE(Diagnostics.find("contiguous region size is not integer typed"),
            std::string::npos);

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  std::string BeforeModule = printModule(*Module);
  std::string BeforeGraph;
  raw_string_ostream BeforeGraphStream(BeforeGraph);
  Graph.print(BeforeGraphStream);

  auto Result = Graph.instantiate(FAM, Backend);
  ASSERT_FALSE(Result);
  EXPECT_NE(toString(Result.takeError()).find("invalid MAIRGraph"),
            std::string::npos);
  EXPECT_EQ(printModule(*Module), BeforeModule);
  EXPECT_EQ(Backend.PrepareCalls, 0u);
  EXPECT_EQ(Backend.EmittedProgramPoints, 0u);
  EXPECT_TRUE(Graph.isLive());
  std::string AfterGraph;
  raw_string_ostream AfterGraphStream(AfterGraph);
  Graph.print(AfterGraphStream);
  EXPECT_EQ(AfterGraph, BeforeGraph);
}

TEST(MAIRMaterialization, RejectsUnavailableContiguousBaseBeforeMutation) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
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

void checkCriticalExitMaterialization(bool RepeatedExit, bool Guarded) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
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
  %last = phi i64 [ %i, %header ], [ 0, %other ]
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
  PHINode *Induction = cast<PHINode>(&Header->front());
  if (RepeatedExit) {
    auto *Branch = cast<BranchInst>(Header->getTerminator());
    auto *Switch = SwitchInst::Create(Branch->getCondition(), Exit, 2);
    Switch->addCase(ConstantInt::getTrue(Ctx), Branch->getSuccessor(0));
    Switch->addCase(ConstantInt::getFalse(Ctx), Exit);
    cast<PHINode>(&Exit->front())->addIncoming(Induction, Header);
    ReplaceInstWithInst(Branch, Switch);
  }
  ASSERT_FALSE(verifyModule(*M, &errs()));
  DominatorTree BeforeDT(F);
  LoopInfo BeforeLI(BeforeDT);
  ASSERT_FALSE(BeforeLI.empty());
  for (const llvm::Loop *Loop : BeforeLI)
    ASSERT_TRUE(Loop->isRecursivelyLCSSAForm(BeforeDT, BeforeLI));

  MAIRGraph Graph(F);
  ProgramPointGuard Guard = AlwaysGuard{};
  if (Guarded)
    Guard = AnyProvenanceExecutedGuard({LoadA, LoadB});
  ProgramPoint &PP = Graph.createEdgeProgramPoint(*Header, *Exit, Guard);
  PP.addObservation(Observation::fromInstruction(*LoadA, M->getDataLayout()));
  ProgramPoint &Second = Graph.createEdgeProgramPoint(*Header, *Exit);
  Second.addObservation(
      Observation::fromInstruction(*LoadB, M->getDataLayout()));
  ASSERT_TRUE(Graph.verify());

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend RejectingBackend;
  RejectingBackend.Reject = true;
  std::string Before = printModule(*M);
  auto Rejected = Graph.instantiate(FAM, RejectingBackend);
  ASSERT_FALSE(Rejected);
  EXPECT_NE(toString(Rejected.takeError()).find("recording backend rejected"),
            std::string::npos);
  EXPECT_EQ(RejectingBackend.PrepareCalls, 1u);
  EXPECT_EQ(RejectingBackend.PreparedUnits, 2u);
  EXPECT_EQ(RejectingBackend.EmittedProgramPoints, 0u);
  EXPECT_EQ(printModule(*M), Before);
  EXPECT_TRUE(Graph.isLive());
  ASSERT_TRUE(Graph.verify());

  RecordingBackend Backend;
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }

  EXPECT_EQ(Result->SplitEdges, 1u);
  EXPECT_EQ(Result->GuardBranches, Guarded ? 1u : 0u);
  EXPECT_EQ(Result->LoweredProgramPoints, 2u);
  EXPECT_TRUE(Result->CFGChanged);
  EXPECT_EQ(Backend.PreparedUnits, 2u);
  EXPECT_TRUE(Backend.SawCompletePlan);
  EXPECT_EQ(Backend.EmittedProgramPoints, 2u);
  EXPECT_TRUE(Graph.isConsumed());
  EXPECT_FALSE(verifyModule(*M, &errs()));
  BasicBlock *Split =
      Header->getTerminator()->getSuccessor(RepeatedExit ? 0 : 1);
  ASSERT_NE(Split, Exit);
  unsigned IncomingCount = RepeatedExit ? 2 : 1;
  EXPECT_EQ(Split->getUniquePredecessor(), Header);
  EXPECT_TRUE(Split->hasNPredecessors(IncomingCount));
  PHINode *LoopExitPhi = nullptr;
  for (PHINode &Phi : Split->phis())
    if (Phi.getType()->isIntegerTy(64))
      LoopExitPhi = &Phi;
  ASSERT_NE(LoopExitPhi, nullptr);
  ASSERT_EQ(LoopExitPhi->getNumIncomingValues(), IncomingCount);
  for (unsigned Index = 0; Index != IncomingCount; ++Index) {
    EXPECT_EQ(LoopExitPhi->getIncomingBlock(Index), Header);
    EXPECT_EQ(LoopExitPhi->getIncomingValue(Index), Induction);
  }
  DominatorTree AfterDT(F);
  LoopInfo AfterLI(AfterDT);
  EXPECT_TRUE(AfterDT.verify());
  if (!Guarded)
    for (const llvm::Loop *Loop : AfterLI)
      EXPECT_TRUE(Loop->isRecursivelyLCSSAForm(AfterDT, AfterLI));

  std::string Text = printModule(*M);
  EXPECT_NE(Text.find("mair.edge"), std::string::npos);
  if (Guarded) {
    EXPECT_NE(Text.find("mair.executed"), std::string::npos);
    EXPECT_NE(Text.find("phi i1 [ false"), std::string::npos);
    EXPECT_NE(Text.find("br i1 %mair.executed"), std::string::npos);
  }
}

TEST(MAIRMaterialization, SplitsCriticalExitAndBuildsAnyProvenanceGuard) {
  checkCriticalExitMaterialization(false, true);
}

TEST(MAIRMaterialization, SplitsRepeatedCriticalExitPreservingLCSSA) {
  checkCriticalExitMaterialization(true, false);
}

TEST(MAIRMaterialization, SplitsRepeatedCriticalExitAndBuildsGuard) {
  checkCriticalExitMaterialization(true, true);
}

TEST(MAIRMaterialization, LowersCompletePeriodicExpressionDag) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
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

  // Both observations share the region, so every expression is lowered once.
  // Min and max become calls to the LLVM intrinsics.
  unsigned NumGEPs = 0;
  DenseMap<Intrinsic::ID, unsigned> NumMinMax;
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      NumGEPs += I.getName().starts_with("mair.expr.gep");
      if (const auto *II = dyn_cast<IntrinsicInst>(&I))
        if (II->getName().starts_with("mair.expr."))
          ++NumMinMax[II->getIntrinsicID()];
    }
  EXPECT_EQ(NumGEPs, 1u);
  EXPECT_EQ(NumMinMax.size(), 4u);
  EXPECT_EQ(NumMinMax.lookup(Intrinsic::smin), 1u);
  EXPECT_EQ(NumMinMax.lookup(Intrinsic::umin), 1u);
  EXPECT_EQ(NumMinMax.lookup(Intrinsic::smax), 1u);
  EXPECT_EQ(NumMinMax.lookup(Intrinsic::umax), 1u);
}

TEST(MAIRMaterialization, LowersExpressionOperandsInOrder) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i32 %a, i32 %b) {
entry:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  Instruction &Return = *F.getEntryBlock().getTerminator();
  Type &I64 = *Type::getInt64Ty(Ctx);

  MAIRGraph Graph(F);
  ProgramPoint &PP = Graph.createProgramPoint(ProgramPointSite::anchored(
      Return, ProgramPoint::SiteKind::BasicBlockExit));
  MAIRScalarExpr WideA =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::ZeroExtend, I64,
                            Graph.createValueExpr(*F.getArg(1)));
  MAIRScalarExpr WideB =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::SignExtend, I64,
                            Graph.createValueExpr(*F.getArg(2)));
  MAIRScalarExpr Size =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::Mul, I64, WideA, WideB);
  AbstractAccess Range(F.getArg(0), AbstractValue::constant(0),
                       AbstractValue());
  Range.setSizeExpr(Size);
  PP.addObservation(Observation({}, Range, AccessKind::Read));
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

  // The emitted instructions must not depend on how the compiler orders the
  // evaluation of call arguments: left operand first, then right, then use.
  std::vector<std::string> Names;
  for (const Instruction &I : F.getEntryBlock())
    if (I.getName().starts_with("mair.expr"))
      Names.push_back(I.getName().str());
  EXPECT_EQ(Names, (std::vector<std::string>{"mair.expr.zext", "mair.expr.sext",
                                             "mair.expr.mul"}));
}

TEST(MAIRMaterialization, RetargetsMultipleProgramPointsAtOneAnchor) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
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

TEST(MAIRMaterialization, MaterializesAllGuardsBeforeBackendChangesCFG) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %first, i1 %second, i1 %bad, i64 %offset) {
entry:
  br i1 %first, label %first.store, label %first.exit
first.store:
  store i32 1, ptr %p
  br label %first.exit
first.exit:
  br label %second.dispatch
second.dispatch:
  br i1 %second, label %second.store, label %second.exit
second.store:
  store i32 2, ptr %p
  br label %second.exit
second.exit:
  br label %finish
finish:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph(F);
  Instruction *Store = nullptr;
  for (BasicBlock &BB : F) {
    if (isa<StoreInst>(BB.front()))
      Store = &BB.front();
    if (BB.getName() != "first.exit" && BB.getName() != "second.exit")
      continue;
    ASSERT_TRUE(Store);
    ProgramPoint &Point = Graph.createProgramPoint(
        ProgramPointSite::anchored(*BB.getTerminator(),
                                   ProgramPoint::SiteKind::BasicBlockExit),
        AnyProvenanceExecutedGuard({Store}));
    Point.addObservation(Observation(
        {Store},
        AbstractAccess(F.getArg(0), AbstractValue::symbol(*F.getArg(4)),
                       AbstractValue::constant(4)),
        AccessKind::Write));
  }
  ASSERT_TRUE(Graph.verify());

  class SplittingBackend final : public MAIRObservationBackend {
  public:
    unsigned Emissions = 0;

    Error prepare(const MAIRMaterializationPlan &Plan) override {
      EXPECT_EQ(Plan.units().size(), 2u);
      return Error::success();
    }

    void emit(const MaterializedProgramPoint &Point,
              IRBuilderBase &Builder) override {
      Instruction *Cursor = &*Builder.GetInsertPoint();
      Function &F = *Cursor->getFunction();
      if (Emissions == 0) {
        unsigned Guards = 0;
        unsigned Addresses = 0;
        for (BasicBlock &BB : F)
          for (Instruction &Inst : BB) {
            if (auto *Branch = dyn_cast<BranchInst>(&Inst))
              Guards += Branch->isConditional() &&
                        isa<PHINode>(Branch->getCondition());
            Addresses += isa<GetElementPtrInst>(Inst);
          }
        EXPECT_EQ(Guards, 2u);
        EXPECT_EQ(Addresses, 2u);
      }
      EXPECT_TRUE(Point.Guarded);
      ASSERT_EQ(Point.Observations.size(), 1u);
      Builder.CreateFreeze(Point.Observations.front().Address,
                           "mair.test.address");
      SplitBlockAndInsertIfThen(F.getArg(3), Cursor, true);
      ++Emissions;
    }
  } Backend;

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }
  EXPECT_EQ(Result->GuardBranches, 2u);
  EXPECT_EQ(Result->LoweredProgramPoints, 2u);
  EXPECT_EQ(Backend.Emissions, 2u);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(MAIRMaterialization,
     PreservesSharedAnchorOrderAcrossGenericAndBackendSplits) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p, i1 %take, i1 %bad) {
entry:
  br i1 %take, label %store, label %exit
store:
  store i32 1, ptr %p
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  Instruction &Store = F.getEntryBlock().getNextNode()->front();
  ProgramPointSite Site = ProgramPointSite::anchored(
      *F.back().getTerminator(), ProgramPoint::SiteKind::BasicBlockExit);
  MAIRGraph Graph(F);
  for (unsigned Index = 0; Index != 2; ++Index)
    Graph.createProgramPoint(Site).addObservation(
        Observation::fromInstruction(Store, M->getDataLayout()));
  Graph.createProgramPoint(Site, AnyProvenanceExecutedGuard({&Store}))
      .addObservation(Observation::fromInstruction(Store, M->getDataLayout()));
  ASSERT_TRUE(Graph.verify());

  class SplittingBackend final : public MAIRObservationBackend {
  public:
    SmallVector<Instruction *, 3> Emissions;
    BranchInst *Guard = nullptr;

    Error prepare(const MAIRMaterializationPlan &) override {
      return Error::success();
    }

    void emit(const MaterializedProgramPoint &Point,
              IRBuilderBase &Builder) override {
      Instruction *Cursor = &*Builder.GetInsertPoint();
      Function &F = *Cursor->getFunction();
      if (Emissions.empty())
        for (BasicBlock &BB : F)
          if (auto *Branch = dyn_cast<BranchInst>(BB.getTerminator()))
            if (Branch->isConditional() && isa<PHINode>(Branch->getCondition()))
              Guard = Branch;
      EXPECT_EQ(Point.Guarded, Emissions.size() == 2);
      Emissions.push_back(cast<Instruction>(Builder.CreateFreeze(
          Point.Observations.front().Address, "mair.test.address")));
      SplitBlockAndInsertIfThen(F.getArg(2), Cursor, true);
    }
  } Backend;

  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }
  EXPECT_EQ(Result->GuardBranches, 1u);
  EXPECT_EQ(Result->LoweredProgramPoints, 3u);
  ASSERT_EQ(Backend.Emissions.size(), 3u);
  ASSERT_TRUE(Backend.Guard);
  DominatorTree DT(F);
  EXPECT_TRUE(DT.dominates(Backend.Emissions[0], Backend.Emissions[1]));
  EXPECT_TRUE(DT.dominates(Backend.Emissions[1], Backend.Guard));
  EXPECT_TRUE(DT.dominates(Backend.Guard->getSuccessor(0),
                           Backend.Emissions[2]->getParent()));
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(MAIRMaterialization, InvalidatesFunctionAnalysesAndIsSingleUse) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
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

TEST(MAIRMaterialization, NestedGraphsPrepareOnceAndConsumeEveryBody) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %before = load i32, ptr %p, align 4
  br label %outer
outer:
  %oi = phi i32 [ 0, %entry ], [ %onext, %outer.latch ]
  %outside.inner = load i32, ptr %p, align 4
  br label %inner
inner:
  %ii = phi i32 [ 0, %outer ], [ %inext, %inner ]
  %inside = load i32, ptr %p, align 4
  %inext = add i32 %ii, 1
  %idone = icmp eq i32 %inext, 2
  br i1 %idone, label %outer.latch, label %inner
outer.latch:
  %after.inner = load i32, ptr %p, align 4
  %onext = add i32 %oi, 1
  %odone = icmp eq i32 %onext, 2
  br i1 %odone, label %exit, label %outer
exit:
  %after = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
  auto Graphs = Graph.graphs();
  ASSERT_EQ(Graphs.size(), 3u);
  PassInstrumentationCallbacks PIC;
  FunctionAnalysisManager FAM;
  registerMaterializationAnalyses(FAM, PIC);
  RecordingBackend Backend;
  auto Result = Graph.instantiate(FAM, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }
  EXPECT_EQ(Backend.PrepareCalls, 1u);
  EXPECT_EQ(Backend.PreparedUnits, 5u);
  EXPECT_EQ(Backend.EmittedProgramPoints, 5u);
  EXPECT_EQ(Backend.EmittedObservations, 5u);
  EXPECT_EQ(Result->LoweredProgramPoints, 5u);
  EXPECT_TRUE(Backend.SawCompletePlan);
  for (const MAIRGraph *Current : Graphs)
    EXPECT_TRUE(Current->isConsumed());
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(MAIRMaterialization, NestedGuardResetsAtEachInnerLoopInvocation) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
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
  MAIRGraph Graph = buildGraph(F);
  for (ProgramPoint *Point : Graph.allProgramPoints())
    while (!Point->empty())
      Point->erase(Point->begin());
  const llvm::Loop *Loop = Graph.getLoopInfo().getLoopFor(Target);
  ASSERT_NE(Loop, nullptr);
  MAIRGraph *Scope = Graph.getLoopGraph(*Loop);
  ASSERT_NE(Scope, nullptr);
  ProgramPoint &Point = Scope->createProgramPoint(
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
  auto M = parseModule(R"IR(
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
  MAIRGraph Graph = buildGraph(F);
  for (ProgramPoint *Point : Graph.allProgramPoints())
    while (!Point->empty())
      Point->erase(Point->begin());
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

TEST(MAIRMaterialization, ZeroOffsetUsesBaseWithoutGEP) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %tmp = alloca i32, align 4
  store i32 1, ptr %tmp, align 4
  %v = load i32, ptr %p, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
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

  EXPECT_EQ(Backend.EmittedObservations, 2u);
  EXPECT_EQ(printModule(*M).find("mair.observation.gep"), std::string::npos);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(MAIRMaterialization, NonZeroOffsetStillLowersGEP) {
  LLVMContext Ctx;
  auto M = parseModule(R"IR(
define void @f(ptr %p) {
entry:
  %q = getelementptr inbounds i8, ptr %p, i64 4
  %v = load i32, ptr %q, align 4
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("f");
  MAIRGraph Graph = buildGraph(F);
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

  EXPECT_EQ(Backend.EmittedObservations, 1u);
  EXPECT_NE(printModule(*M).find("mair.observation.gep"), std::string::npos);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

class ConstantSizeBackend final : public MAIRObservationBackend {
public:
  SmallVector<WeakTrackingVH, 2> AccessSizes;

  Error prepare(const MAIRMaterializationPlan &) override {
    return Error::success();
  }

  void emit(const MaterializedProgramPoint &Point, IRBuilderBase &) override {
    for (const MaterializedObservation &Obs : Point.Observations) {
      EXPECT_FALSE(Obs.isPeriodic());
      AccessSizes.emplace_back(Obs.AccessSize);
    }
  }
};

void expectConstantAccessSize(Function &Function, ConstantSizeBackend &Backend,
                              uint64_t Input, uint64_t ExpectedSize) {
  Argument *Count = Function.getArg(1);
  Count->replaceAllUsesWith(ConstantInt::get(Count->getType(), Input));
  const DataLayout &Layout = Function.getParent()->getDataLayout();
  for (BasicBlock &Block : Function)
    for (Instruction &Instruction : Block)
      if (!Instruction.getType()->isVoidTy())
        if (Constant *Folded = ConstantFoldInstruction(&Instruction, Layout))
          Instruction.replaceAllUsesWith(Folded);
  ASSERT_EQ(Backend.AccessSizes.size(), 1u);
  auto *Size = dyn_cast_or_null<ConstantInt>(
      static_cast<Value *>(Backend.AccessSizes.front()));
  ASSERT_NE(Size, nullptr);
  EXPECT_EQ(Size->getValue().getZExtValue(), ExpectedSize);
  EXPECT_FALSE(verifyModule(*Function.getParent(), &errs()));
}

void expectLoopAccessSize(StringRef IR, uint64_t Input, uint64_t ExpectedSize) {
  LLVMContext Context;
  auto Module = parseModule(IR, Context);
  ASSERT_TRUE(Module);
  Function &Function = *Module->getFunction("f");
  PassInstrumentationCallbacks Callbacks;
  FunctionAnalysisManager Analyses;
  registerLoopAnalyses(Analyses, Callbacks);
  MAIRGraph Graph = createGraphWithFunctionAnalyses(Function, Analyses);
  MAIRGraphAnalysisManager GraphAnalyses;
  registerMAIRGraphAnalyses(GraphAnalyses, &Callbacks, &Analyses);
  EXPECT_FALSE(runPassOnGraphTree(LoopPeriodicObservationCombinePass(), Graph,
                                  GraphAnalyses)
                   .areAllPreserved());
  ASSERT_TRUE(Graph.verify());
  for (MAIRGraph *Scope : Graph.graphs())
    GraphAnalyses.clear(*Scope, Function.getName());
  ConstantSizeBackend Backend;
  auto Result = Graph.instantiate(Analyses, Backend);
  if (!Result) {
    ADD_FAILURE() << toString(Result.takeError());
    return;
  }
  expectConstantAccessSize(Function, Backend, Input, ExpectedSize);
}

TEST(MAIRMaterialization, WidensSymbolicLoopAccessSizeBeforeMultiplication) {
  const char *IR = R"IR(
define void @f(ptr %base, i8 %count) {
entry:
  br label %header
header:
  %index = phi i8 [ 0, %entry ], [ %next, %body ]
  %more = icmp ult i8 %index, %count
  br i1 %more, label %body, label %exit
body:
  %wide = zext i8 %index to i64
  %address = getelementptr inbounds i32, ptr %base, i64 %wide
  %value = load i32, ptr %address, align 4
  %next = add nuw i8 %index, 1
  br label %header
exit:
  ret void
}
)IR";
  for (auto [Count, ExpectedSize] : {std::pair<uint64_t, uint64_t>{0, 0},
                                     {1, 4},
                                     {127, 508},
                                     {128, 512},
                                     {255, 1020}}) {
    SCOPED_TRACE(Count);
    expectLoopAccessSize(IR, Count, ExpectedSize);
  }
}

TEST(MAIRMaterialization, PreservesFullNarrowCounterLoopAccessSize) {
  expectLoopAccessSize(R"IR(
define void @f(ptr %base, i8 %count) {
entry:
  br label %loop
loop:
  %index = phi i8 [ 0, %entry ], [ %next, %loop ]
  %wide = zext i8 %index to i64
  %address = getelementptr inbounds i32, ptr %base, i64 %wide
  %value = load i32, ptr %address, align 4
  %next = add i8 %index, 1
  %done = icmp eq i8 %next, %count
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
)IR",
                       0, 1024);
}

TEST(MAIRMaterialization, WidensMergedExpressionAccessSizeBeforeAddition) {
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    for (auto [Count, ExpectedSize] :
         {std::pair<uint64_t, uint64_t>{0, 400}, {200, 600}, {255, 655}}) {
      SCOPED_TRACE(Count);
      LLVMContext Context;
      auto Module = parseModule(R"IR(
define void @f(ptr %base, i8 %count) {
entry:
  ret void
}
)IR",
                                Context);
      ASSERT_TRUE(Module);
      Function &Function = *Module->getFunction("f");
      MAIRGraph Graph(Function);
      Type &SizeType = *IntegerType::get(Context, 9);
      MAIRScalarExpr CountExpr = Graph.createValueExpr(*Function.getArg(1));
      MAIRScalarExpr Extended = Graph.createUnaryExpr(
          MAIRScalarExpr::Kind::ZeroExtend, SizeType, CountExpr);
      MAIRScalarExpr Constant =
          Graph.createConstantExpr(SizeType, APInt(9, 200));
      MAIRScalarExpr Size = Graph.createBinaryExpr(
          MAIRScalarExpr::Kind::Add, SizeType, Constant, Extended);
      AbstractAccess First(Function.getArg(0), AbstractValue::constant(0),
                           AbstractValue());
      First.setSizeExpr(Size);
      AbstractAccess Second(Function.getArg(0), AbstractValue::constant(200),
                            AbstractValue());
      Second.setSizeExpr(Size);
      auto Merged =
          Reverse
              ? AbstractAccess::merge(Second, First, nullptr, nullptr, &Graph)
              : AbstractAccess::merge(First, Second, nullptr, nullptr, &Graph);
      ASSERT_TRUE(Merged);
      EXPECT_EQ(Merged->getOffset(), AbstractValue::constant(0));
      ProgramPoint &Point = Graph.createProgramPoint(
          ProgramPointSite::anchored(*Function.getEntryBlock().getTerminator(),
                                     ProgramPoint::SiteKind::BasicBlockExit));
      Point.addObservation(
          Observation({}, MemoryRegion(*Merged), AccessKind::Read));
      ASSERT_TRUE(Graph.verify());
      PassInstrumentationCallbacks Callbacks;
      FunctionAnalysisManager Analyses;
      registerMaterializationAnalyses(Analyses, Callbacks);
      ConstantSizeBackend Backend;
      auto Result = Graph.instantiate(Analyses, Backend);
      if (!Result) {
        ADD_FAILURE() << toString(Result.takeError());
        continue;
      }
      expectConstantAccessSize(Function, Backend, Count, ExpectedSize);
    }
  }
}

TEST(MAIRMaterialization, RejectsWrappingMultiplyExpressionSizeMerge) {
  LLVMContext Context;
  auto Module = parseModule(R"IR(
define void @f(ptr %base, i8 %count) {
entry:
  ret void
}
)IR",
                            Context);
  ASSERT_TRUE(Module);
  Function &Function = *Module->getFunction("f");
  MAIRGraph Graph(Function);
  Type &SizeType = *IntegerType::get(Context, 9);
  MAIRScalarExpr Count = Graph.createValueExpr(*Function.getArg(1));
  MAIRScalarExpr Extended =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::ZeroExtend, SizeType, Count);
  MAIRScalarExpr One = Graph.createConstantExpr(SizeType, APInt(9, 1));
  MAIRScalarExpr Five = Graph.createConstantExpr(SizeType, APInt(9, 5));
  MAIRScalarExpr Sum = Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add,
                                              SizeType, One, Extended);
  MAIRScalarExpr Size =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::Mul, SizeType, Five, Sum);
  AbstractAccess First(Function.getArg(0), AbstractValue::constant(0),
                       AbstractValue());
  First.setSizeExpr(Size);
  AbstractAccess Second(Function.getArg(0), AbstractValue::constant(5),
                        AbstractValue());
  Second.setSizeExpr(Size);
  EXPECT_FALSE(AbstractAccess::merge(First, Second, nullptr, nullptr, &Graph));
  EXPECT_FALSE(AbstractAccess::merge(Second, First, nullptr, nullptr, &Graph));
}

TEST(MAIRMaterialization, RejectsWrappingAddExpressionSizeMerge) {
  LLVMContext Context;
  auto Module = parseModule(R"IR(
define void @f(ptr %base, i7 %count) {
entry:
  ret void
}
)IR",
                            Context);
  ASSERT_TRUE(Module);
  Function &Function = *Module->getFunction("f");
  MAIRGraph Graph(Function);
  Type &SizeType = *Type::getInt8Ty(Context);
  MAIRScalarExpr Count = Graph.createValueExpr(*Function.getArg(1));
  MAIRScalarExpr Extended =
      Graph.createUnaryExpr(MAIRScalarExpr::Kind::ZeroExtend, SizeType, Count);
  MAIRScalarExpr Hundred = Graph.createConstantExpr(SizeType, APInt(8, 100));
  MAIRScalarExpr Sum = Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add,
                                              SizeType, Hundred, Extended);
  MAIRScalarExpr Size =
      Graph.createBinaryExpr(MAIRScalarExpr::Kind::Add, SizeType, Sum, Hundred);
  AbstractAccess First(Function.getArg(0), AbstractValue::constant(0),
                       AbstractValue());
  First.setSizeExpr(Size);
  AbstractAccess Second(Function.getArg(0), AbstractValue::constant(100),
                        AbstractValue());
  Second.setSizeExpr(Size);
  EXPECT_FALSE(AbstractAccess::merge(First, Second, nullptr, nullptr, &Graph));
  EXPECT_FALSE(AbstractAccess::merge(Second, First, nullptr, nullptr, &Graph));
}

} // namespace
