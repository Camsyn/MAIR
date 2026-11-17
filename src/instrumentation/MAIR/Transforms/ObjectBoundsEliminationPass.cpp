#include "MAIR/Transforms/ObjectBoundsEliminationPass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIRStatLog.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Instructions.h"

#include <optional>

using namespace llvm;

#define DEBUG_TYPE "mair-object-bounds-elimination"

STATISTIC(NumInputObservations,
          "Number of observations entering object bounds elimination");
STATISTIC(NumEliminatedObservations,
          "Number of observations within a trusted object eliminated");

namespace mair {

namespace {

/// LazyValueInfo of the function, built on first use: SCEV alone proves or
/// rejects most observations.
class LazyLVI {
  Function &F;
  AssumptionCache &AC;
  std::optional<LazyValueInfo> LVI;

public:
  LazyLVI(Function &F, AssumptionCache &AC) : F(F), AC(AC) {}

  LazyValueInfo &get() {
    if (!LVI)
      LVI.emplace(&AC, &F.getDataLayout());
    return *LVI;
  }
};

/// Proves bounds of integer SCEVs where a program point runs. SCEV comes
/// first: its ranges, those under the guards of the loop around the point,
/// and the conditions that dominate the point. LVI comes second, for the
/// values the SCEV is made of.
class BoundsProver {
  ScalarEvolution &SE;
  LazyLVI &LVI;
  Instruction &Context;
  const Loop *ContextLoop;
  bool UsedLVI = false;

  /// A range of the value of \p U where Context runs: LVI sees the
  /// instruction that defines it and the conditions that lead to Context.
  ConstantRange getLeafRange(const SCEVUnknown &U) {
    Value *V = U.getValue();
    if (!V->getType()->isIntegerTy())
      return ConstantRange::getFull(SE.getTypeSizeInBits(U.getType()));
    // Undef must not count as a value of LVI's choice: the program runs with
    // whatever value it has.
    return LVI.get().getConstantRange(V, &Context, /*UndefAllowed=*/false);
  }

  /// A range of \p S built from the ranges of its operands down to the values
  /// it is made of, so that LVI's facts about those values reach \p S.
  /// SCEV's own range of each node bounds the result too.
  ConstantRange getComposedRange(const SCEV *S) {
    ConstantRange Own = SE.getUnsignedRange(S).intersectWith(
        SE.getSignedRange(S), ConstantRange::Signed);
    if (S->getType()->isPointerTy())
      return Own;
    unsigned Width = Own.getBitWidth();
    auto Fold = [&](auto Combine) {
      ArrayRef<const SCEV *> Operands = S->operands();
      ConstantRange Result = getComposedRange(Operands.front());
      for (const SCEV *Operand : Operands.drop_front())
        Result = Combine(Result, getComposedRange(Operand));
      return Result;
    };
    ConstantRange Composed = ConstantRange::getFull(Width);
    switch (S->getSCEVType()) {
    case scConstant:
      return ConstantRange(cast<SCEVConstant>(S)->getAPInt());
    case scUnknown:
      Composed = getLeafRange(*cast<SCEVUnknown>(S));
      break;
    case scTruncate:
      Composed =
          getComposedRange(cast<SCEVCastExpr>(S)->getOperand()).truncate(Width);
      break;
    case scZeroExtend:
      Composed = getComposedRange(cast<SCEVCastExpr>(S)->getOperand())
                     .zeroExtend(Width);
      break;
    case scSignExtend:
      Composed = getComposedRange(cast<SCEVCastExpr>(S)->getOperand())
                     .signExtend(Width);
      break;
    case scAddExpr:
      Composed = Fold([](const ConstantRange &L, const ConstantRange &R) {
        return L.add(R);
      });
      break;
    case scMulExpr:
      Composed = Fold([](const ConstantRange &L, const ConstantRange &R) {
        return L.multiply(R);
      });
      break;
    case scUDivExpr:
      Composed = Fold([](const ConstantRange &L, const ConstantRange &R) {
        return L.udiv(R);
      });
      break;
    case scSMaxExpr:
      Composed = Fold([](const ConstantRange &L, const ConstantRange &R) {
        return L.smax(R);
      });
      break;
    case scUMaxExpr:
      Composed = Fold([](const ConstantRange &L, const ConstantRange &R) {
        return L.umax(R);
      });
      break;
    case scSMinExpr:
      Composed = Fold([](const ConstantRange &L, const ConstantRange &R) {
        return L.smin(R);
      });
      break;
    case scUMinExpr:
    case scSequentialUMinExpr:
      Composed = Fold([](const ConstantRange &L, const ConstantRange &R) {
        return L.umin(R);
      });
      break;
    default:
      break;
    }
    return Composed.intersectWith(Own, ConstantRange::Signed);
  }

public:
  BoundsProver(ScalarEvolution &SE, LazyLVI &LVI, Instruction &Context,
               const Loop *ContextLoop)
      : SE(SE), LVI(LVI), Context(Context), ContextLoop(ContextLoop) {}

  /// Whether some bound needed LVI.
  bool usedLVI() const { return UsedLVI; }

  /// Whether Lo <= S <= Hi, compared as signed or unsigned integers of the
  /// width of \p S.
  bool isWithin(const SCEV *S, const APInt &Lo, const APInt &Hi, bool Signed) {
    unsigned Width = SE.getTypeSizeInBits(S->getType());
    APInt L = Signed ? Lo.sextOrTrunc(Width) : Lo.zextOrTrunc(Width);
    APInt H = Signed ? Hi.sextOrTrunc(Width) : Hi.zextOrTrunc(Width);
    bool LowOK = false, HighOK = false;
    auto Fits = [&](const ConstantRange &R) {
      LowOK |= Signed ? R.getSignedMin().sge(L) : R.getUnsignedMin().uge(L);
      HighOK |= Signed ? R.getSignedMax().sle(H) : R.getUnsignedMax().ule(H);
      return LowOK && HighOK;
    };
    auto RangeOf = [&](const SCEV *X) {
      return Signed ? SE.getSignedRange(X) : SE.getUnsignedRange(X);
    };

    if (Fits(RangeOf(S)))
      return true;
    if (ContextLoop && Fits(RangeOf(SE.applyLoopGuards(S, ContextLoop))))
      return true;
    auto Holds = [&](ICmpInst::Predicate P, const APInt &C) {
      return SE.isKnownPredicateAt(P, S, SE.getConstant(C), &Context);
    };
    LowOK = LowOK || Holds(Signed ? ICmpInst::ICMP_SGE : ICmpInst::ICMP_UGE, L);
    HighOK =
        HighOK || Holds(Signed ? ICmpInst::ICMP_SLE : ICmpInst::ICMP_ULE, H);
    if (LowOK && HighOK)
      return true;
    if (!Fits(getComposedRange(S)))
      return false;
    UsedLVI = true;
    return true;
  }
};

/// The SCEV of a scalar expression of the graph, or null if its operand
/// types do not fit what ScalarEvolution builds.
const SCEV *getSCEV(ScalarEvolution &SE, const MAIRScalarExpr &Expr) {
  using Kind = MAIRScalarExpr::Kind;
  Type *Ty = Expr.getType();
  switch (Expr.getKind()) {
  case Kind::Constant:
    return SE.getConstant(Expr.getConstant());
  case Kind::LLVMValue: {
    Value *V = const_cast<Value *>(Expr.getLLVMValue());
    if (!V->getType()->isIntegerTy() && !V->getType()->isPointerTy())
      return nullptr;
    return SE.getSCEV(V);
  }
  default:
    break;
  }

  SmallVector<const SCEV *, 2> Operands;
  for (const MAIRScalarExpr &Operand : Expr.operands()) {
    const SCEV *S = getSCEV(SE, Operand);
    if (!S)
      return nullptr;
    Operands.push_back(S);
  }
  auto AllOfType = [&](Type *T) {
    return all_of(Operands, [&](const SCEV *S) { return S->getType() == T; });
  };
  switch (Expr.getKind()) {
  case Kind::Truncate:
  case Kind::ZeroExtend:
  case Kind::SignExtend: {
    if (Operands.size() != 1 || !Ty->isIntegerTy() ||
        !Operands.front()->getType()->isIntegerTy())
      return nullptr;
    unsigned From = SE.getTypeSizeInBits(Operands.front()->getType());
    unsigned To = SE.getTypeSizeInBits(Ty);
    if (Expr.getKind() == Kind::Truncate)
      return From > To ? SE.getTruncateExpr(Operands.front(), Ty) : nullptr;
    if (From >= To)
      return nullptr;
    return Expr.getKind() == Kind::ZeroExtend
               ? SE.getZeroExtendExpr(Operands.front(), Ty)
               : SE.getSignExtendExpr(Operands.front(), Ty);
  }
  case Kind::Add:
    if (Operands.size() != 2)
      return nullptr;
    if (Ty->isPointerTy()) {
      // A pointer plus an offset of its index width.
      if (Operands[0]->getType()->isPointerTy() ==
          Operands[1]->getType()->isPointerTy())
        return nullptr;
      const SCEV *Pointer =
          Operands[0]->getType()->isPointerTy() ? Operands[0] : Operands[1];
      const SCEV *Offset = Pointer == Operands[0] ? Operands[1] : Operands[0];
      if (Offset->getType() != SE.getEffectiveSCEVType(Pointer->getType()))
        return nullptr;
      return SE.getAddExpr(Pointer, Offset);
    }
    return AllOfType(Ty) ? SE.getAddExpr(Operands[0], Operands[1]) : nullptr;
  case Kind::Mul:
    return Operands.size() == 2 && Ty->isIntegerTy() && AllOfType(Ty)
               ? SE.getMulExpr(Operands[0], Operands[1])
               : nullptr;
  case Kind::UDiv:
    return Operands.size() == 2 && Ty->isIntegerTy() && AllOfType(Ty)
               ? SE.getUDivExpr(Operands[0], Operands[1])
               : nullptr;
  case Kind::SMin:
  case Kind::UMin:
  case Kind::SMax:
  case Kind::UMax:
    if (Operands.size() != 2 || !Ty->isIntegerTy() || !AllOfType(Ty))
      return nullptr;
    switch (Expr.getKind()) {
    case Kind::SMin:
      return SE.getSMinExpr(Operands[0], Operands[1]);
    case Kind::UMin:
      return SE.getUMinExpr(Operands[0], Operands[1]);
    case Kind::SMax:
      return SE.getSMaxExpr(Operands[0], Operands[1]);
    default:
      return SE.getUMaxExpr(Operands[0], Operands[1]);
    }
  default:
    return nullptr;
  }
}

/// The byte offset of \p Address from \p Base, or null.
const SCEV *getOffsetFrom(ScalarEvolution &SE, const SCEV *Address,
                          const Value &Base) {
  if (!Address || !Address->getType()->isPointerTy())
    return nullptr;
  const SCEV *Offset =
      SE.getMinusSCEV(Address, SE.getUnknown(const_cast<Value *>(&Base)));
  return isa<SCEVCouldNotCompute>(Offset) ? nullptr : Offset;
}

/// Extend each SCEV in \p Integers to one integer type of at least
/// \p MinWidth bits, signed ones by sign extension, the others by zero
/// extension, and return that type.
Type *extendToCommonType(ScalarEvolution &SE, LLVMContext &Ctx,
                         unsigned MinWidth,
                         ArrayRef<std::pair<const SCEV **, bool>> Integers) {
  unsigned Width = MinWidth;
  for (auto [S, IsSigned] : Integers)
    Width = std::max<unsigned>(Width, SE.getTypeSizeInBits((*S)->getType()));
  Type *Ty = IntegerType::get(Ctx, Width);
  for (auto [S, IsSigned] : Integers)
    *S = IsSigned ? SE.getNoopOrSignExtend(*S, Ty)
                  : SE.getNoopOrZeroExtend(*S, Ty);
  return Ty;
}

/// Whether the region of \p Obs lies within [Base, Base + ObjectSize) where
/// its program point runs.
bool isWithinObject(const Observation &Obs, uint64_t ObjectSize,
                    ScalarEvolution &SE, BoundsProver &Prover) {
  const Value &Base = *Obs.getBase();
  if (!Base.getType()->isPointerTy())
    return false;
  LLVMContext &Ctx = Base.getContext();
  unsigned IndexWidth =
      SE.getTypeSizeInBits(SE.getEffectiveSCEVType(Base.getType()));
  // Every value a check below accepts is below 2^63 in magnitude once the
  // object fits in 2^32 bytes, so ScalarEvolution's wrapping arithmetic is
  // exact on them.
  if (IndexWidth < 64 || ObjectSize > (uint64_t(1) << 32))
    return false;

  if (const AbstractAccess *Region = Obs.getContiguousRegion()) {
    const SCEV *Offset = getOffsetFrom(
        SE, Region->getAddressSCEV(SE, AbstractAccess::ZeroOffsetBase::Opaque),
        Base);
    if (!Offset)
      return false;
    const SCEV *Size = nullptr;
    if (Region->hasSizeExpr())
      Size = getSCEV(SE, Region->getSizeExpr());
    else if (Region->getSize().isConstant())
      Size = SE.getConstant(Offset->getType(), Region->getSize().getConstant());
    else if (Region->getSize().isSymbol() &&
             Region->getSize().getSymbol()->getType()->isIntegerTy())
      Size = SE.getSCEV(const_cast<Value *>(Region->getSize().getSymbol()));
    if (!Size || !Size->getType()->isIntegerTy())
      return false;

    Type *Ty = extendToCommonType(SE, Ctx, IndexWidth,
                                  {{&Offset, true}, {&Size, false}});
    APInt Zero(Ty->getIntegerBitWidth(), 0);
    APInt End(Ty->getIntegerBitWidth(), ObjectSize);
    // 0 <= Offset <= N and Size <= N, so Offset + Size does not wrap.
    return Prover.isWithin(Offset, Zero, End, /*Signed=*/true) &&
           Prover.isWithin(Size, Zero, End, /*Signed=*/false) &&
           Prover.isWithin(SE.getAddExpr(Offset, Size), Zero, End,
                           /*Signed=*/true);
  }

  if (const PeriodicRegion *Region = Obs.getPeriodicRegion()) {
    const SCEV *Start =
        getOffsetFrom(SE, getSCEV(SE, Region->getStart()), Base);
    const SCEV *Stride = getSCEV(SE, Region->getStride());
    const SCEV *Size = getSCEV(SE, Region->getAccessSize());
    const SCEV *Trips = getSCEV(SE, Region->getTripCount());
    if (!Start || !Stride || !Size || !Trips ||
        !Stride->getType()->isIntegerTy() || !Size->getType()->isIntegerTy() ||
        !Trips->getType()->isIntegerTy())
      return false;
    Type *Ty = extendToCommonType(
        SE, Ctx, IndexWidth,
        {{&Start, true}, {&Stride, true}, {&Size, false}, {&Trips, false}});
    unsigned Width = Ty->getIntegerBitWidth();
    APInt Zero(Width, 0), End(Width, ObjectSize);
    APInt Limit = APInt::getOneBitSet(Width, 31);
    // With at most 2^31 trips of a stride of at most 2^31 bytes, every trip
    // starts between the first and the last one without wrapping, so the
    // region is within [0, N) if both of those trips are. Zero trips touch
    // nothing; the checks then merely may fail to prove that.
    if (!Prover.isWithin(Trips, Zero, Limit, /*Signed=*/false) ||
        !Prover.isWithin(Stride, -Limit, Limit, /*Signed=*/true) ||
        !Prover.isWithin(Size, Zero, End, /*Signed=*/false))
      return false;
    const SCEV *Last = SE.getAddExpr(
        Start, SE.getMulExpr(SE.getMinusSCEV(Trips, SE.getOne(Ty)), Stride));
    for (const SCEV *Trip : {Start, Last})
      if (!Prover.isWithin(Trip, Zero, End, /*Signed=*/true) ||
          !Prover.isWithin(SE.getAddExpr(Trip, Size), Zero, End,
                           /*Signed=*/true))
        return false;
    return true;
  }
  return false;
}

/// The instruction where the checks of \p PP would run.
Instruction &getContext(const ProgramPoint &PP) {
  const ProgramPointSite &Site = PP.getSite();
  if (Site.isAnchored())
    return const_cast<Instruction &>(Site.getAnchor());
  return *const_cast<BasicBlock *>(Site.getEdgeFrom())->getTerminator();
}

} // namespace

PreservedAnalyses
ObjectBoundsEliminationPass::run(MAIRGraph &G, MAIRGraphAnalysisManager &AM) {
  const MAIROptimizationConfig &Config = G.getOptimizationConfig();
  if (!Config.trustsObjects() || !G.getMutableFunction())
    return PreservedAnalyses::all();
  MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
  if (!Loops.isAvailable())
    return PreservedAnalyses::all();

  MAIRStatScope Scope("boe", Ordinal, G, NumInputObservations, [&] {
    return MAIRStat::compute(G).getNumObservations();
  });
  ScalarEvolution &SE = Loops.getSE();
  LazyLVI LVI(*G.getMutableFunction(), Loops.getAssumptionCache());

  SmallVector<Observation *, 8> Eliminated;
  for (ProgramPoint &PP : G) {
    Instruction &Context = getContext(PP);
    const Loop *ContextLoop = G.getLoopInfo().getLoopFor(Context.getParent());
    for (Observation &Obs : PP) {
      if (!Obs.isDeletable() || !Obs.getBase())
        continue;
      std::optional<uint64_t> ObjectSize =
          Config.getTrustedObjectSize(*Obs.getBase());
      if (!ObjectSize)
        continue;
      BoundsProver Prover(SE, LVI, Context, ContextLoop);
      if (!isWithinObject(Obs, *ObjectSize, SE, Prover))
        continue;
      Eliminated.push_back(&Obs);
      MAIRStatScope::add(Prover.usedLVI() ? MAIRStatField::Lvi
                                          : MAIRStatField::Scev);
    }
  }

  for (Observation *Obs : Eliminated)
    Obs->eraseFromParent();
  NumEliminatedObservations += Eliminated.size();
  MAIRStatScope::add(MAIRStatField::Eliminated, Eliminated.size());
  return Eliminated.empty() ? PreservedAnalyses::all()
                            : PreservedAnalyses::none();
}

} // namespace mair
