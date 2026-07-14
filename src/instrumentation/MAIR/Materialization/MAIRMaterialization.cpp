#include "MAIR/Materialization/MAIRMaterialization.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

using namespace llvm;

namespace mair {
namespace {

Error materializationError(const Twine &Message) {
  return createStringError(inconvertibleErrorCode(), Message);
}

struct EdgeCursor {
  const BasicBlock *From = nullptr;
  const BasicBlock *To = nullptr;
  Instruction *Cursor = nullptr;
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

bool valueAvailableAt(const Value &V, const Instruction &At,
                      DominatorTree &DT) {
  if (isa<Constant>(&V) || isa<Argument>(&V) || isa<GlobalValue>(&V))
    return true;
  const auto *I = dyn_cast<Instruction>(&V);
  return I && DT.dominates(I, &At);
}

bool abstractValueAvailableAt(const AbstractValue &V, const Instruction &At,
                              DominatorTree &DT) {
  if (V.isUnknown())
    return false;
  if (V.isConstant())
    return true;
  return valueAvailableAt(*V.getSymbol(), At, DT);
}

bool expressionAvailableAt(const MAIRScalarExpr &Expr, const Instruction &At,
                           DominatorTree &DT) {
  if (!Expr.isValid())
    return false;
  if (Expr.getKind() == MAIRScalarExpr::Kind::LLVMValue &&
      !valueAvailableAt(*Expr.getLLVMValue(), At, DT))
    return false;
  return llvm::all_of(Expr.operands(), [&](const MAIRScalarExpr &Operand) {
    return expressionAvailableAt(Operand, At, DT);
  });
}

bool instructionPrecedes(const Instruction &Before, const Instruction &After) {
  if (Before.getParent() != After.getParent())
    return false;
  return Before.comesBefore(&After);
}

bool validateGuard(const Observation &Obs, const Instruction &Target,
                   std::string &Failure) {
  const auto *Guard = std::get_if<AnyProvenanceExecutedGuard>(&Obs.getGuard());
  if (!Guard)
    return true;

  for (Instruction *I : Guard->getProvenance()) {
    if (!I || I->getFunction() != Target.getFunction()) {
      Failure = "execution guard provenance is outside the target function";
      return false;
    }
    if (I->getParent() == Target.getParent() &&
        !instructionPrecedes(*I, Target)) {
      Failure = "execution guard provenance does not precede its sink";
      return false;
    }
  }
  return true;
}

bool validateAccess(const MemoryAccess &Access, const Instruction &Target,
                    DominatorTree &DT, std::string &Failure) {
  if (const AbstractAccess *Region = Access.getContiguousRegion()) {
    const Value *Base = Region->getBase();
    const AbstractValue &Offset = Region->getOffset();
    const AbstractValue &Size = Region->getSize();
    bool OffsetIsCompletePointer =
        Offset.isSymbol() && Offset.getSymbol()->getType()->isPointerTy();
    bool OffsetHasMaterializableType =
        !Offset.isSymbol() || OffsetIsCompletePointer ||
        Offset.getSymbol()->getType()->isIntegerTy();
    bool SizeHasMaterializableType =
        !Size.isSymbol() || Size.getSymbol()->getType()->isIntegerTy();
    if (!Base || !Base->getType()->isPointerTy() ||
        (!OffsetIsCompletePointer && !valueAvailableAt(*Base, Target, DT)) ||
        !OffsetHasMaterializableType || !SizeHasMaterializableType ||
        !abstractValueAvailableAt(Region->getOffset(), Target, DT) ||
        !abstractValueAvailableAt(Region->getSize(), Target, DT)) {
      Failure = "contiguous region has an unavailable or unknown value";
      return false;
    }
    return true;
  }

  const PeriodicRegion *Region = Access.getPeriodicRegion();
  if (!Region || !Region->getBase() ||
      !expressionAvailableAt(Region->getStart(), Target, DT) ||
      !expressionAvailableAt(Region->getStride(), Target, DT) ||
      !expressionAvailableAt(Region->getAccessSize(), Target, DT) ||
      !expressionAvailableAt(Region->getTripCount(), Target, DT)) {
    Failure = "periodic region has an unavailable or invalid expression";
    return false;
  }
  return true;
}

Expected<MAIRMaterializationPlan> buildPlan(const MAIRGraph &Graph, Function &F,
                                            DominatorTree &DT, LoopInfo &LI) {
  MAIRMaterializationPlan Plan;
  if (!Graph.isLive())
    return materializationError("cannot instantiate an already consumed graph");
  if (!Graph.verify())
    return materializationError("cannot instantiate an invalid MAIRGraph");

  for (const Observation &Obs : Graph) {
    if (Obs.empty())
      continue;

    const Instruction *Target = nullptr;
    if (Obs.isEdgeSite()) {
      const BasicBlock *From = Obs.getEdgeFrom();
      const BasicBlock *To = Obs.getEdgeTo();
      if (!From || !To || From->getParent() != &F || To->getParent() != &F)
        return materializationError("edge site is outside the graph function");
      unsigned SuccNo = GetSuccessorNumber(From, To);
      if (!isCriticalEdge(From->getTerminator(), SuccNo,
                          /*AllowIdenticalEdges=*/true) ||
          !isPatchBLoopExit(*From, *To, LI))
        return materializationError(
            "unsupported edge site: expected a critical Patch B loop exit");
      Target = From->getTerminator();
    } else {
      const BasicBlock *BB = Obs.getBasicBlock();
      const Instruction *Anchor = Obs.getAnchor();
      if (!BB || !Anchor || BB->getParent() != &F || Anchor->getParent() != BB)
        return materializationError("anchored observation has a stale anchor");
      BasicBlock::const_iterator FirstInsertion = BB->getFirstInsertionPt();
      if (FirstInsertion == BB->end() ||
          (Anchor != &*FirstInsertion && !FirstInsertion->comesBefore(Anchor)))
        return materializationError(
            "anchored observation has no legal insertion point");
      Target = Anchor;
    }

    std::string Failure;
    if (!validateGuard(Obs, *Target, Failure))
      return materializationError(Failure);
    for (const MemoryAccess &Access : Obs)
      if (!validateAccess(Access, *Target, DT, Failure))
        return materializationError(Failure);

    MaterializationUnit Unit;
    Unit.Source = &Obs;
    Unit.Site = Obs.getSite();
    Unit.Guard = Obs.getGuard();
    for (const MemoryAccess &Access : Obs)
      Unit.Accesses.push_back({&Access, Access.getKind(), Access.getRegion()});
    Plan.addUnit(std::move(Unit));
  }
  return Plan;
}

Value *lowerExpr(const MAIRScalarExpr &Expr, IRBuilder<> &Builder,
                 DenseMap<const void *, Value *> &Cache) {
  const void *Key = Expr.getOpaqueNode();
  if (Value *Cached = Cache.lookup(Key))
    return Cached;

  using Kind = MAIRScalarExpr::Kind;
  Value *Result = nullptr;
  switch (Expr.getKind()) {
  case Kind::Constant:
    Result = ConstantInt::get(Expr.getType(), Expr.getConstant());
    break;
  case Kind::LLVMValue:
    Result = const_cast<Value *>(Expr.getLLVMValue());
    break;
  case Kind::Add: {
    Value *LHS = lowerExpr(Expr.operands()[0], Builder, Cache);
    Value *RHS = lowerExpr(Expr.operands()[1], Builder, Cache);
    if (Expr.getType()->isPointerTy())
      Result = Builder.CreateGEP(Type::getInt8Ty(Builder.getContext()), LHS,
                                 RHS, "mair.expr.gep");
    else
      Result = Builder.CreateAdd(LHS, RHS, "mair.expr.add");
    break;
  }
  case Kind::Mul:
    Result = Builder.CreateMul(lowerExpr(Expr.operands()[0], Builder, Cache),
                               lowerExpr(Expr.operands()[1], Builder, Cache),
                               "mair.expr.mul");
    break;
  case Kind::UDiv:
    Result = Builder.CreateUDiv(lowerExpr(Expr.operands()[0], Builder, Cache),
                                lowerExpr(Expr.operands()[1], Builder, Cache),
                                "mair.expr.udiv");
    break;
  case Kind::Truncate:
    Result = Builder.CreateTrunc(lowerExpr(Expr.operands()[0], Builder, Cache),
                                 Expr.getType(), "mair.expr.trunc");
    break;
  case Kind::ZeroExtend:
    Result = Builder.CreateZExt(lowerExpr(Expr.operands()[0], Builder, Cache),
                                Expr.getType(), "mair.expr.zext");
    break;
  case Kind::SignExtend:
    Result = Builder.CreateSExt(lowerExpr(Expr.operands()[0], Builder, Cache),
                                Expr.getType(), "mair.expr.sext");
    break;
  case Kind::SMin:
  case Kind::UMin:
  case Kind::SMax:
  case Kind::UMax: {
    Value *LHS = lowerExpr(Expr.operands()[0], Builder, Cache);
    Value *RHS = lowerExpr(Expr.operands()[1], Builder, Cache);
    bool IsSigned =
        Expr.getKind() == Kind::SMin || Expr.getKind() == Kind::SMax;
    bool IsMin = Expr.getKind() == Kind::SMin || Expr.getKind() == Kind::UMin;
    StringRef KindName = "umin";
    switch (Expr.getKind()) {
    case Kind::SMin:
      KindName = "smin";
      break;
    case Kind::UMin:
      KindName = "umin";
      break;
    case Kind::SMax:
      KindName = "smax";
      break;
    case Kind::UMax:
      KindName = "umax";
      break;
    default:
      llvm_unreachable("not a min/max expression");
    }
    Value *Compare =
        IsSigned
            ? Builder.CreateICmpSLT(LHS, RHS,
                                    (Twine("mair.expr.") + KindName + ".cmp"))
            : Builder.CreateICmpULT(LHS, RHS,
                                    (Twine("mair.expr.") + KindName + ".cmp"));
    if (!IsMin)
      Compare = IsSigned
                    ? Builder.CreateICmpSGT(
                          LHS, RHS, (Twine("mair.expr.") + KindName + ".cmp"))
                    : Builder.CreateICmpUGT(
                          LHS, RHS, (Twine("mair.expr.") + KindName + ".cmp"));
    Result = Builder.CreateSelect(Compare, LHS, RHS,
                                  (Twine("mair.expr.") + KindName));
    break;
  }
  }
  Cache[Key] = Result;
  return Result;
}

Value *lowerAbstractValue(const AbstractValue &Value, IRBuilder<> &Builder) {
  if (Value.isConstant())
    return ConstantInt::get(Type::getInt64Ty(Builder.getContext()),
                            Value.getConstant());
  return const_cast<class Value *>(Value.getSymbol());
}

Value *lowerContiguous(const AbstractAccess &Region, IRBuilder<> &Builder) {
  Value *Base = const_cast<Value *>(Region.getBase());
  if (Region.getOffset().isSymbol() &&
      Region.getOffset().getSymbol()->getType()->isPointerTy())
    return const_cast<Value *>(Region.getOffset().getSymbol());
  Value *Offset = lowerAbstractValue(Region.getOffset(), Builder);
  return Builder.CreateGEP(Type::getInt8Ty(Builder.getContext()), Base, Offset,
                           "mair.access.gep");
}

Value *materializeGuard(const ObservationGuard &ObservationGuard,
                        BasicBlock *Target, Function &F) {
  SmallVector<PHINode *, 8> InsertedPhis;
  SSAUpdater Updater(&InsertedPhis);
  Updater.Initialize(Type::getInt1Ty(F.getContext()), "mair.executed");
  SmallPtrSet<BasicBlock *, 8> Seen;
  const auto &Guard = std::get<AnyProvenanceExecutedGuard>(ObservationGuard);
  bool EntryHasProvenance = false;
  for (Instruction *I : Guard.getProvenance())
    if (I->getParent() == &F.getEntryBlock())
      EntryHasProvenance = true;
    else if (Seen.insert(I->getParent()).second)
      Updater.AddAvailableValue(I->getParent(),
                                ConstantInt::getTrue(F.getContext()));
  Updater.AddAvailableValue(&F.getEntryBlock(),
                            EntryHasProvenance
                                ? ConstantInt::getTrue(F.getContext())
                                : ConstantInt::getFalse(F.getContext()));
  return Updater.GetValueInMiddleOfBlock(Target);
}

EdgeCursor *findEdgeCursor(SmallVectorImpl<EdgeCursor> &Cursors,
                           const BasicBlock *From, const BasicBlock *To) {
  for (EdgeCursor &Cursor : Cursors)
    if (Cursor.From == From && Cursor.To == To)
      return &Cursor;
  return nullptr;
}

} // namespace

Expected<MAIRInstantiationResult>
MAIRGraph::instantiate(FunctionAnalysisManager &FAM,
                       MAIRObservationBackend &Backend) {
  if (!isLive())
    return materializationError("graph already consumed");
  if (!MutableF)
    return materializationError(
        "instantiate requires a graph built from a mutable Function");
  Function &F = *MutableF;

  // Preflight owns its analysis snapshots and finishes before any IR mutation.
  DominatorTree PreflightDT(F);
  LoopInfo PreflightLI(PreflightDT);
  Expected<MAIRMaterializationPlan> PlanOrErr =
      buildPlan(*this, F, PreflightDT, PreflightLI);
  if (!PlanOrErr)
    return PlanOrErr.takeError();
  MAIRMaterializationPlan Plan = std::move(*PlanOrErr);
  if (Error Err = Backend.prepare(Plan))
    return std::move(Err);

  DominatorTree DT(F);
  PostDominatorTree PDT(F);
  LoopInfo LI(DT);
  DomTreeUpdater DTU(DT, PDT, DomTreeUpdater::UpdateStrategy::Eager);
  SmallVector<EdgeCursor, 8> EdgeCursors;
  MAIRInstantiationResult Result;

  for (const MaterializationUnit &Unit : Plan.units()) {
    if (!Unit.Site.isEdge())
      continue;
    const BasicBlock *From = Unit.Site.getEdgeFrom();
    const BasicBlock *To = Unit.Site.getEdgeTo();
    if (findEdgeCursor(EdgeCursors, From, To))
      continue;
    unsigned SuccNo = GetSuccessorNumber(From, To);
    CriticalEdgeSplittingOptions Options(&DT, &LI, nullptr, &PDT);
    Options.setPreserveLCSSA().setMergeIdenticalEdges();
    BasicBlock *VirtualExit =
        SplitKnownCriticalEdge(const_cast<Instruction *>(From->getTerminator()),
                               SuccNo, Options, "mair.edge");
    if (!VirtualExit)
      report_fatal_error(
          "Patch C edge split failed after successful preflight");
    EdgeCursors.push_back({From, To, VirtualExit->getTerminator()});
    ++Result.SplitEdges;
    Result.CFGChanged = true;
  }

  for (const MaterializationUnit &Unit : Plan.units()) {
    Instruction *Cursor = nullptr;
    if (Unit.Site.isEdge()) {
      EdgeCursor *Edge = findEdgeCursor(EdgeCursors, Unit.Site.getEdgeFrom(),
                                        Unit.Site.getEdgeTo());
      assert(Edge && "preflight edge must have an apply cursor");
      Cursor = Edge->Cursor;
    } else {
      Cursor = const_cast<Instruction *>(Unit.Site.getAnchor());
    }

    IRBuilder<> UnguardedBuilder(Cursor);
    IRBuilder<> *BuilderPtr = &UnguardedBuilder;
    std::optional<IRBuilder<>> GuardBuilder;
    bool IsGuarded =
        std::holds_alternative<AnyProvenanceExecutedGuard>(Unit.Guard);
    if (IsGuarded) {
      Value *Guard = materializeGuard(Unit.Guard, Cursor->getParent(), F);
      Instruction *ThenTerminator = SplitBlockAndInsertIfThen(
          Guard, Cursor, /*Unreachable=*/false, nullptr, &DTU, &LI, nullptr);
      GuardBuilder.emplace(ThenTerminator);
      BuilderPtr = &*GuardBuilder;
      ++Result.GuardBranches;
      Result.CFGChanged = true;
    }

    SmallVector<MaterializedAccess, 4> Lowered;
    for (const PlannedAccess &Access : Unit.Accesses) {
      MaterializedAccess Materialized;
      Materialized.Source = Access.Source;
      Materialized.Kind = Access.Kind;
      DenseMap<const void *, Value *> Cache;
      if (const auto *Contiguous =
              std::get_if<AbstractAccess>(&Access.Region)) {
        Materialized.Address = lowerContiguous(*Contiguous, *BuilderPtr);
        Materialized.AccessSize =
            lowerAbstractValue(Contiguous->getSize(), *BuilderPtr);
      } else {
        const PeriodicRegion &Periodic =
            std::get<PeriodicRegion>(Access.Region);
        Materialized.Address =
            lowerExpr(Periodic.getStart(), *BuilderPtr, Cache);
        Materialized.Stride =
            lowerExpr(Periodic.getStride(), *BuilderPtr, Cache);
        Materialized.AccessSize =
            lowerExpr(Periodic.getAccessSize(), *BuilderPtr, Cache);
        Materialized.TripCount =
            lowerExpr(Periodic.getTripCount(), *BuilderPtr, Cache);
      }
      Lowered.push_back(Materialized);
    }

    MaterializedObservation Materialized{Unit.Source, Unit.Site.getKind(),
                                         IsGuarded, Lowered};
    Backend.emit(Materialized, *BuilderPtr);
    ++Result.LoweredObservations;
  }

  if (verifyFunction(F, &errs()))
    report_fatal_error("Patch C produced invalid LLVM IR");
  FAM.invalidate(F, PreservedAnalyses::none());
  markConsumed();
  return Result;
}

} // namespace mair
