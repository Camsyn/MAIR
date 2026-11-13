#include "MAIR/Materialization/MAIRMaterialization.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

using namespace llvm;

namespace mair {
namespace {

struct PendingProgramPoint {
  const MaterializationUnit *Unit;
  Instruction *Cursor;
  bool Guarded;
  SmallVector<MaterializedObservation, 4> Observations;
};

bool isPatchBLoopExit(const BasicBlock &From, const BasicBlock &To,
                      LoopInfo &LI) {
  for (Loop *L = LI.getLoopFor(&From); L; L = L->getParentLoop()) {
    if (L->contains(&To))
      continue;
    if (L->getUniqueExitBlock() == &To && L->getExitingBlock() == &From)
      return true;
  }
  return false;
}

bool expressionAvailableAt(const MAIRScalarExpr &Expr, const Instruction &At,
                           DominatorTree &DT) {
  if (!Expr.isValid())
    return false;
  if (Expr.getKind() == MAIRScalarExpr::Kind::LLVMValue &&
      !isValueAvailableAt(*Expr.getLLVMValue(), At, DT))
    return false;
  return llvm::all_of(Expr.operands(), [&](const MAIRScalarExpr &Operand) {
    return expressionAvailableAt(Operand, At, DT);
  });
}

Error validateGuard(const ProgramPoint &PP, const Instruction &Target) {
  const auto *Guard = std::get_if<AnyProvenanceExecutedGuard>(&PP.getGuard());
  if (!Guard)
    return Error::success();
  if (const BasicBlock *Reset = Guard->getResetBlock())
    if (Reset->getParent() != Target.getFunction())
      return createStringError(
          "execution guard reset is outside the target function");

  for (Instruction *I : Guard->getProvenance()) {
    if (!I || I->getFunction() != Target.getFunction())
      return createStringError(
          "execution guard provenance is outside the target function");
    if (I->getParent() == Target.getParent() && !I->comesBefore(&Target))
      return createStringError(
          "execution guard provenance does not precede its sink");
  }
  return Error::success();
}

Error validateObservation(const Observation &Obs, const Instruction &Target,
                          DominatorTree &DT) {
  auto ValidateBounds = [&](const AbstractAccess &Region) -> Error {
    const Value *Base = Region.getBase();
    const AbstractValue &Offset = Region.getOffset();
    const AbstractValue &Size = Region.getSize();
    bool OffsetIsCompletePointer =
        Offset.isSymbol() && Offset.getSymbol()->getType()->isPointerTy();
    bool OffsetHasMaterializableType =
        !Offset.isSymbol() || OffsetIsCompletePointer ||
        Offset.getSymbol()->getType()->isIntegerTy();
    bool SizeHasMaterializableType = Region.hasSizeExpr() || !Size.isSymbol() ||
                                     Size.getSymbol()->getType()->isIntegerTy();
    bool SizeAvailable =
        Region.hasSizeExpr()
            ? expressionAvailableAt(Region.getSizeExpr(), Target, DT)
            : isValueAvailableAt(Size, Target, DT);
    if (!Base || !Base->getType()->isPointerTy() ||
        (!OffsetIsCompletePointer && !isValueAvailableAt(*Base, Target, DT)) ||
        !OffsetHasMaterializableType || !SizeHasMaterializableType ||
        !isValueAvailableAt(Offset, Target, DT) || !SizeAvailable)
      return createStringError(
          "contiguous region has an unavailable or unknown value");
    return Error::success();
  };

  if (const AbstractAccess *Region = Obs.getContiguousRegion())
    return ValidateBounds(*Region);

  if (const MaskedRegion *Region = Obs.getMaskedRegion()) {
    if (Error Err = ValidateBounds(Region->getBounds()))
      return Err;
    if (const Value *Mask = Region->getDynamicMask())
      if (!isValueAvailableAt(*Mask, Target, DT))
        return createStringError("masked region has an unavailable mask");
    return Error::success();
  }

  const PeriodicRegion *Region = Obs.getPeriodicRegion();
  if (!Region || !Region->getBase() ||
      !expressionAvailableAt(Region->getStart(), Target, DT) ||
      !expressionAvailableAt(Region->getStride(), Target, DT) ||
      !expressionAvailableAt(Region->getAccessSize(), Target, DT) ||
      !expressionAvailableAt(Region->getTripCount(), Target, DT))
    return createStringError(
        "periodic region has an unavailable or invalid expression");
  return Error::success();
}

Expected<MAIRMaterializationPlan> buildPlan(const MAIRGraph &Graph, Function &F,
                                            DominatorTree &DT, LoopInfo &LI) {
  if (!Graph.verify())
    return createStringError("cannot instantiate an invalid MAIRGraph");

  MAIRMaterializationPlan Plan;
  for (const ProgramPoint *Point : Graph.allProgramPoints()) {
    const ProgramPoint &PP = *Point;
    if (PP.empty())
      continue;

    const Instruction *Target = nullptr;
    if (PP.isEdgeSite()) {
      const BasicBlock *From = PP.getEdgeFrom();
      const BasicBlock *To = PP.getEdgeTo();
      if (!From || !To || From->getParent() != &F || To->getParent() != &F)
        return createStringError("edge site is outside the graph function");
      unsigned SuccNo = GetSuccessorNumber(From, To);
      if (!isCriticalEdge(From->getTerminator(), SuccNo,
                          /*AllowIdenticalEdges=*/true) ||
          !isPatchBLoopExit(*From, *To, LI))
        return createStringError(
            "unsupported edge site: expected a critical Patch B loop exit");
      Target = &PP.getAnchor();
    } else {
      const BasicBlock *BB = PP.getBasicBlock();
      const Instruction &Anchor = PP.getAnchor();
      if (!BB || BB->getParent() != &F || Anchor.getParent() != BB)
        return createStringError("anchored program point has a stale anchor");
      BasicBlock::const_iterator FirstInsertion = BB->getFirstInsertionPt();
      if (FirstInsertion == BB->end() ||
          (&Anchor != &*FirstInsertion &&
           !FirstInsertion->comesBefore(&Anchor)))
        return createStringError(
            "anchored program point has no legal insertion point");
      Target = &Anchor;
    }

    if (Error Err = validateGuard(PP, *Target))
      return std::move(Err);
    for (const Observation &Obs : PP)
      if (Error Err = validateObservation(Obs, *Target, DT))
        return std::move(Err);

    MaterializationUnit Unit{&PP, PP.getSite(), PP.getGuard(), {}};
    for (const Observation &Obs : PP)
      Unit.Observations.push_back(
          {&Obs, Obs.getKind(), Obs.getProperties(), Obs.getRegion()});
    Plan.addUnit(std::move(Unit));
  }
  return Plan;
}

Value *lowerExpr(const MAIRScalarExpr &Expr, IRBuilder<> &Builder,
                 DenseMap<const void *, Value *> &Cache) {
  const void *Key = Expr.getOpaqueNode();
  if (Value *Cached = Cache.lookup(Key))
    return Cached;

  // Lower the operands first and in order. As call arguments their order,
  // and with it the order of the emitted instructions, would be unspecified.
  SmallVector<Value *, 2> Ops;
  for (const MAIRScalarExpr &Operand : Expr.operands())
    Ops.push_back(lowerExpr(Operand, Builder, Cache));

  using Kind = MAIRScalarExpr::Kind;
  Type *Ty = Expr.getType();
  Value *Result = nullptr;
  switch (Expr.getKind()) {
  case Kind::Constant:
    Result = ConstantInt::get(Ty, Expr.getConstant());
    break;
  case Kind::LLVMValue:
    Result = const_cast<Value *>(Expr.getLLVMValue());
    break;
  case Kind::Add:
    // A pointer-typed add is a pointer plus a byte offset.
    Result = Ty->isPointerTy()
                 ? Builder.CreateGEP(Type::getInt8Ty(Builder.getContext()),
                                     Ops[0], Ops[1], "mair.expr.gep")
                 : Builder.CreateAdd(Ops[0], Ops[1], "mair.expr.add");
    break;
  case Kind::Mul:
    Result = Builder.CreateMul(Ops[0], Ops[1], "mair.expr.mul");
    break;
  case Kind::UDiv:
    Result = Builder.CreateUDiv(Ops[0], Ops[1], "mair.expr.udiv");
    break;
  case Kind::Truncate:
    Result = Builder.CreateTrunc(Ops[0], Ty, "mair.expr.trunc");
    break;
  case Kind::ZeroExtend:
    Result = Builder.CreateZExt(Ops[0], Ty, "mair.expr.zext");
    break;
  case Kind::SignExtend:
    Result = Builder.CreateSExt(Ops[0], Ty, "mair.expr.sext");
    break;
  case Kind::SMin:
    Result = Builder.CreateBinaryIntrinsic(Intrinsic::smin, Ops[0], Ops[1], {},
                                           "mair.expr.smin");
    break;
  case Kind::UMin:
    Result = Builder.CreateBinaryIntrinsic(Intrinsic::umin, Ops[0], Ops[1], {},
                                           "mair.expr.umin");
    break;
  case Kind::SMax:
    Result = Builder.CreateBinaryIntrinsic(Intrinsic::smax, Ops[0], Ops[1], {},
                                           "mair.expr.smax");
    break;
  case Kind::UMax:
    Result = Builder.CreateBinaryIntrinsic(Intrinsic::umax, Ops[0], Ops[1], {},
                                           "mair.expr.umax");
    break;
  }
  Cache[Key] = Result;
  return Result;
}

Value *lowerAbstractValue(const AbstractValue &V, IRBuilder<> &Builder) {
  if (V.isConstant())
    return ConstantInt::get(Type::getInt64Ty(Builder.getContext()),
                            V.getConstant());
  return const_cast<Value *>(V.getSymbol());
}

Value *lowerContiguous(const AbstractAccess &Region, IRBuilder<> &Builder) {
  Value *Base = const_cast<Value *>(Region.getBase());
  const AbstractValue &Offset = Region.getOffset();
  if (Offset == 0)
    return Base;
  if (Offset.isSymbol() && Offset.getSymbol()->getType()->isPointerTy())
    return const_cast<Value *>(Offset.getSymbol());
  return Builder.CreateGEP(Type::getInt8Ty(Builder.getContext()), Base,
                           lowerAbstractValue(Offset, Builder),
                           "mair.observation.gep");
}

// A basic block has no internal control flow. If every provenance instruction
// is in the insertion block, that block's execution is exactly the guard, so
// the branch would be constant true and should not be created.
bool provenanceExecutesWithTarget(const ProgramPointGuard &Guard,
                                  const BasicBlock *Target) {
  const auto *Executed = std::get_if<AnyProvenanceExecutedGuard>(&Guard);
  if (!Executed || !Target)
    return false;
  ArrayRef<Instruction *> Provenance = Executed->getProvenance();
  if (Provenance.empty())
    return false;
  return llvm::all_of(Provenance, [&](const Instruction *I) {
    return I && I->getParent() == Target;
  });
}

Value *materializeGuard(const ProgramPointGuard &PPGuard, BasicBlock *Target,
                        Function &F) {
  LLVMContext &Ctx = F.getContext();
  SmallVector<PHINode *, 8> InsertedPhis;
  SSAUpdater Updater(&InsertedPhis);
  Updater.Initialize(Type::getInt1Ty(Ctx), "mair.executed");
  SmallPtrSet<BasicBlock *, 8> Seen;
  const auto &Guard = std::get<AnyProvenanceExecutedGuard>(PPGuard);
  BasicBlock *Reset = const_cast<BasicBlock *>(Guard.getResetBlock());
  if (Reset) {
    Updater.AddAvailableValue(Reset, ConstantInt::getFalse(Ctx));
    Seen.insert(Reset);
  }
  BasicBlock *Entry = &F.getEntryBlock();
  bool EntryHasProvenance = false;
  for (Instruction *I : Guard.getProvenance())
    if (I->getParent() == Entry)
      EntryHasProvenance = true;
    else if (Seen.insert(I->getParent()).second)
      Updater.AddAvailableValue(I->getParent(), ConstantInt::getTrue(Ctx));
  if (Reset != Entry)
    Updater.AddAvailableValue(Entry,
                              ConstantInt::getBool(Ctx, EntryHasProvenance));
  return Updater.GetValueInMiddleOfBlock(Target);
}

MaterializedObservation
lowerObservation(const PlannedObservation &Obs, IRBuilder<> &Builder,
                 DenseMap<const void *, Value *> &Cache) {
  MaterializedObservation Materialized;
  Materialized.Source = Obs.Source;
  Materialized.Kind = Obs.Kind;
  Materialized.Properties = Obs.Properties;
  if (const auto *Contiguous = std::get_if<AbstractAccess>(&Obs.Region)) {
    Materialized.Address = lowerContiguous(*Contiguous, Builder);
    Materialized.AccessSize =
        Contiguous->hasSizeExpr()
            ? lowerExpr(Contiguous->getSizeExpr(), Builder, Cache)
            : lowerAbstractValue(Contiguous->getSize(), Builder);
  } else if (const auto *Masked = std::get_if<MaskedRegion>(&Obs.Region)) {
    Materialized.Address = lowerContiguous(Masked->getBounds(), Builder);
    Materialized.AccessSize =
        lowerAbstractValue(Masked->getBounds().getSize(), Builder);
    Materialized.MaskElementSize = Masked->getElementSize();
    if (Masked->isConstantMask()) {
      SmallVector<Constant *, 8> Elements;
      for (unsigned Lane = 0; Lane < Masked->getNumLanes(); ++Lane)
        Elements.push_back(ConstantInt::getBool(
            Builder.getContext(), Masked->getConstantMask().test(Lane)));
      Materialized.Mask = ConstantVector::get(Elements);
    } else {
      Materialized.Mask = const_cast<Value *>(Masked->getDynamicMask());
    }
  } else {
    const PeriodicRegion &Periodic = std::get<PeriodicRegion>(Obs.Region);
    Materialized.Address = lowerExpr(Periodic.getStart(), Builder, Cache);
    Materialized.Stride = lowerExpr(Periodic.getStride(), Builder, Cache);
    Materialized.AccessSize =
        lowerExpr(Periodic.getAccessSize(), Builder, Cache);
    Materialized.TripCount = lowerExpr(Periodic.getTripCount(), Builder, Cache);
  }
  return Materialized;
}

} // namespace

Expected<MAIRInstantiationResult>
MAIRGraph::instantiate(FunctionAnalysisManager &FAM,
                       MAIRObservationBackend &Backend) {
  if (getParent())
    return createStringError("instantiate requires the root MAIRGraph");
  if (!isLive())
    return createStringError("graph already consumed");
  if (!MutableF)
    return createStringError(
        "instantiate requires a graph built from a mutable Function");
  Function &F = *MutableF;

  // Preflight owns its analysis snapshots and finishes before any IR mutation.
  DominatorTree DT(F);
  LoopInfo LI(DT);
  Expected<MAIRMaterializationPlan> PlanOrErr =
      buildPlan(*this, F, DT, LI);
  if (!PlanOrErr)
    return PlanOrErr.takeError();
  MAIRMaterializationPlan Plan = std::move(*PlanOrErr);
  if (Error Err = Backend.prepare(Plan))
    return std::move(Err);

  DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Eager);
  // The cursor of each split edge is the terminator of its new exit block.
  SmallDenseMap<std::pair<const BasicBlock *, const BasicBlock *>,
                Instruction *, 8>
      EdgeCursors;
  SmallVector<PendingProgramPoint, 8> Pending;
  MAIRInstantiationResult Result;

  for (const MaterializationUnit &Unit : Plan.units()) {
    if (!Unit.Site.isEdge())
      continue;
    const BasicBlock *From = Unit.Site.getEdgeFrom();
    const BasicBlock *To = Unit.Site.getEdgeTo();
    if (EdgeCursors.contains({From, To}))
      continue;
    unsigned SuccNo = GetSuccessorNumber(From, To);
    CriticalEdgeSplittingOptions Options(&DT, &LI);
    Options.setPreserveLCSSA().setMergeIdenticalEdges();
    BasicBlock *VirtualExit =
        SplitKnownCriticalEdge(const_cast<Instruction *>(From->getTerminator()),
                               SuccNo, Options, "mair.edge");
    if (!VirtualExit)
      report_fatal_error(
          "Patch C edge split failed after successful preflight");
    EdgeCursors[{From, To}] = VirtualExit->getTerminator();
    ++Result.SplitEdges;
    Result.CFGChanged = true;
  }

  for (const MaterializationUnit &Unit : Plan.units()) {
    Instruction *Cursor = nullptr;
    if (Unit.Site.isEdge()) {
      Cursor =
          EdgeCursors.lookup({Unit.Site.getEdgeFrom(), Unit.Site.getEdgeTo()});
      assert(Cursor && "preflight edge must have an apply cursor");
    } else {
      Cursor = const_cast<Instruction *>(&Unit.Site.getAnchor());
    }

    Instruction *InsertPt = Cursor;
    BasicBlock *Head = Cursor->getParent();
    bool IsGuarded =
        std::holds_alternative<AnyProvenanceExecutedGuard>(Unit.Guard) &&
        !provenanceExecutesWithTarget(Unit.Guard, Head);
    if (IsGuarded) {
      Value *Guard = materializeGuard(Unit.Guard, Head, F);
      InsertPt = SplitBlockAndInsertIfThen(Guard, Cursor, /*Unreachable=*/false,
                                           nullptr, &DTU, &LI);
      // The split moves Cursor into the tail block. Earlier points pending at
      // Cursor emit at the head's new branch, ahead of this guard.
      for (PendingProgramPoint &Point : Pending)
        if (Point.Cursor == Cursor)
          Point.Cursor = Head->getTerminator();
      ++Result.GuardBranches;
      Result.CFGChanged = true;
    }

    IRBuilder<> Builder(InsertPt);
    SmallVector<MaterializedObservation, 4> Lowered;
    DenseMap<const void *, Value *> ExprCache;
    for (const PlannedObservation &Obs : Unit.Observations)
      Lowered.push_back(lowerObservation(Obs, Builder, ExprCache));

    Pending.push_back({&Unit, InsertPt, IsGuarded, std::move(Lowered)});
  }

  for (const PendingProgramPoint &Point : Pending) {
    IRBuilder<> Builder(Point.Cursor);
    MaterializedProgramPoint PP{Point.Unit->Source, Point.Unit->Site.getKind(),
                                Point.Guarded, Point.Observations};
    Backend.emit(PP, Builder);
    ++Result.LoweredProgramPoints;
  }

  if (verifyFunction(F, &errs()))
    report_fatal_error("Patch C produced invalid LLVM IR");
  FAM.invalidate(F, PreservedAnalyses::none());
  markConsumed();
  return Result;
}

} // namespace mair
