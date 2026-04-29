//===------------Instrumentation.cpp - TransformUtils Infrastructure ------===//
//
// This file is to provide some instrumentation utilities.
// These utilities usually come from the subsequent versions of LLVM, i.e., LLVM
// 15+.
//
//===----------------------------------------------------------------------===//
//
// This file defines the common initialization infrastructure for the
// Instrumentation library.
//
//===----------------------------------------------------------------------===//

#include "Instrumentation.h"
#include "Utils/Logging.h"
#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"
#include "Utils/ValueUtils.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Process.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <cstdint>
#include <optional>

#if LLVM_VERSION_MAJOR >= 20
#include "llvm/TargetParser/Triple.h"
#else
#include "llvm/ADT/Triple.h"
#endif

#include "xsan_llvm_compat.h"

using namespace llvm;

#define DEBUG_TYPE "xsan_opt"

#if LLVM_VERSION_MAJOR <= 15
// Backport https://github.com/llvm/llvm-project/pull/99439
static cl::opt<bool> ClIgnoreRedundantInstrumentation(
    "ignore-redundant-instrumentation",
    cl::desc("Ignore redundant instrumentation"), cl::Hidden, cl::init(false));

namespace {
/// Diagnostic information for IR instrumentation reporting.
class DiagnosticInfoInstrumentation : public DiagnosticInfo {
  const Twine &Msg;

public:
  DiagnosticInfoInstrumentation(const Twine &DiagMsg,
                                DiagnosticSeverity Severity = DS_Warning)
      : DiagnosticInfo(DK_Linker, Severity), Msg(DiagMsg) {}
  void print(DiagnosticPrinter &DP) const override { DP << Msg; }
};
} // namespace

namespace __xsan {

/// Check if module has flag attached, if not add the flag.
bool checkIfAlreadyInstrumented(Module &M, StringRef Flag) {
  if (!M.getModuleFlag(Flag)) {
    M.addModuleFlag(Module::ModFlagBehavior::Override, Flag, 1);
    return false;
  }
  if (ClIgnoreRedundantInstrumentation)
    return true;
  std::string diagInfo =
      "Redundant instrumentation detected, with module flag: " +
      std::string(Flag);
  M.getContext().diagnose(
      DiagnosticInfoInstrumentation(diagInfo, DiagnosticSeverity::DS_Warning));
  return true;
}
} // namespace __xsan
#endif

/// Number of loop invariant checks relocated.
uint32_t NumInvChecksRelocated = 0;
uint32_t NumInvChecksRelocatedDup = 0;
/// Number of loop periodic checks combined.
uint32_t NumPeriodChecksCombined = 0;
uint32_t NumPeriodChecksCombinedDup = 0;

namespace {

/// IntTy -> IntTy, PtrTy -> IntTy with the same bitwidth.
static IntegerType *getIntTy(Type *Ty, const DataLayout &DL) {
  if (auto *ITy = dyn_cast<IntegerType>(Ty))
    return ITy;
  if (auto *PTy = dyn_cast<PointerType>(Ty)) {
    unsigned PtrBitWidth = DL.getPointerSizeInBits(PTy->getAddressSpace());
    return IntegerType::get(Ty->getContext(), PtrBitWidth);
  }
  return nullptr;
}

/// Metch standard patterns as follows:
///    ( $EXT1(Inv1 * $EXT2($AddRec)) + Inv2 )
/// -> ( $EXT1(Inv1 * $EXT2({Start,+,Step})) + Inv2 )
/// which can be rewritten to:
///   { $EXT1(Inv1 * $EXT2(Start)) + Inv2, +, $EXT1(Inv1 * $EXT2(Step)) }
/// In fact, the two are not equivalent, they have different cycles.
/// $Ext = zext or sext,  Inv1/Inv2 = loop invariants
/// Note that Ext1/Ext2 might be no-op, Inv1 might be 1, Inv2 must not be
/// zero. Therefore the standard pattern could be expanded into:
///   ($AddRec + $Invariant)
///   ($EXT($AddRec) + $Invariant)
///   (($AddRec * $Inv1) + $Inv2)
///   ($EXT($AddRec * $Inv1) + $Inv2)
///   (($EXT($AddRec) * $Inv1) + $Inv2)
/// e.g.,
///   ((zext i32 {%0,+,1}<%5> to i64) + %1)<nuw>
///   ((8 * (sext i32 {%out.0381,+,1}<%while.cond> to i64))<nsw> + %147)
///   ({(8 * (sext i32 %i51.2 to i64))<nsw>,+,8}<nsw><%while.body189> + %244)
/// Rewrite the AddExpr satifying the above standard form into a AddRecExpr,
/// and further return it with the bytewidth of the inner AddRec.
using __xsan::LoopInvariantChecker;
class PtrAddExpr2PtrAddRecRewriter {
public:
  static std::optional<PtrAddExpr2PtrAddRecRewriter>
  get(const SCEV *S, ScalarEvolution &SE, LoopInvariantChecker &LIC) {
    const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(S);

    PtrAddExpr2PtrAddRecRewriter Rewriter(SE, LIC);
    if (!Rewriter.matchAddExpr(S)) {
      return std::nullopt;
    }

    return Rewriter;
  }

  IntegerType *getCounterTy() const {
    return getIntTy(CounterTy, SE.getDataLayout());
  }

  const SCEV *getBasePtr() const { return Inv2Add; }
  const SCEV *getBaseOffset() const { return Start; }

  /// { $EXT1(Inv1 * $EXT2(Start)) + Inv2, +, $EXT1(Inv1 * $EXT2(Step)) }
  std::optional<const SCEVAddRecExpr *> assembleNewAddRec() {
    if (!Start || !Step || !L)
      return std::nullopt;

    auto NewStart = assembleNewStart();
    auto NewStep = assembleNewStep();
    if (!NewStart || !NewStep)
      return std::nullopt;

    /// TODO: can we derive the new flags?
    const SCEV *NewAddRec =
        SE.getAddRecExpr(*NewStart, *NewStep, L, SCEV::FlagAnyWrap);
    return (const SCEVAddRecExpr *)NewAddRec;
  }

private:
  PtrAddExpr2PtrAddRecRewriter(ScalarEvolution &SE, LoopInvariantChecker &LIC)
      : SE(SE), LIC(LIC) {}

  //  $EXT1(Inv1 * $EXT2(Step))
  std::optional<const SCEV *> assembleNewStep() {
    if (!Step)
      return std::nullopt;

    const SCEV *NewStep = Step;
    if (InnerExt) {
      NewStep =
          SE.getCastExpr(InnerExt->getSCEVType(), NewStep, InnerExt->getType());
    }

    if (Inv2Mul) {
      NewStep = SE.getMulExpr(NewStep, Inv2Mul);
    }

    if (OuterExt) {
      NewStep =
          SE.getCastExpr(OuterExt->getSCEVType(), NewStep, OuterExt->getType());
    }

    return NewStep;
  }

  // $EXT1(Inv1 * $EXT2(Start)) + Inv2
  std::optional<const SCEV *> assembleNewStart() {
    if (!Start)
      return std::nullopt;

    const SCEV *NewStart = Start;
    if (InnerExt) {
      NewStart = SE.getCastExpr(InnerExt->getSCEVType(), NewStart,
                                InnerExt->getType());
    }

    if (Inv2Mul) {
      NewStart = SE.getMulExpr(NewStart, Inv2Mul);
    }

    if (OuterExt) {
      NewStart = SE.getCastExpr(OuterExt->getSCEVType(), NewStart,
                                OuterExt->getType());
    }

    NewStart = SE.getAddExpr(NewStart, Inv2Add);

    return NewStart;
  }

  /// Match ( ... + Inv2 )
  bool matchAddExpr(const SCEV *S) {
    const SCEVAddExpr *Add = dyn_cast<SCEVAddExpr>(S);
    if (!Add || Add->getNumOperands() != 2)
      return false;
    const auto *LHS = Add->getOperand(0);
    const auto *RHS = Add->getOperand(1);
    if (matchOuterExt(LHS)) {
      Inv2Add = RHS;
      return LIC.isLoopInvariant(SE, Inv2Add, L);
    }
    reset();
    if (matchOuterExt(RHS)) {
      Inv2Add = LHS;
      return LIC.isLoopInvariant(SE, Inv2Add, L);
    }
    return false;
  }

  /// Match $EXT1(...)
  bool matchOuterExt(const SCEV *S) {
    const SCEVZeroExtendExpr *ZExt = dyn_cast<SCEVZeroExtendExpr>(S);
    if (ZExt) {
      OuterExt = ZExt;
    }
    return matchMul(ZExt ? ZExt->getOperand() : S);
  }

  /// Match (Inv1 * ...)
  bool matchMul(const SCEV *S) {
    const SCEVMulExpr *Mul = dyn_cast<SCEVMulExpr>(S);
    if (!Mul) {
      return matchInnerExt(S);
    }
    if (Mul->getNumOperands() != 2) {
      return false;
    }
    const auto *LHS = Mul->getOperand(0);
    const auto *RHS = Mul->getOperand(1);
    if (matchInnerExt(RHS)) {
      Inv2Mul = LHS;
      return LIC.isLoopInvariant(SE, Inv2Mul, L);
    }
    if (matchInnerExt(LHS)) {
      Inv2Mul = RHS;
      return LIC.isLoopInvariant(SE, Inv2Mul, L);
    }
    return false;
  }

  /// Match $EXT2(...)
  bool matchInnerExt(const SCEV *S) {
    const SCEVIntegralCastExpr *Cast = dyn_cast<SCEVIntegralCastExpr>(S);
    if (Cast) {
      InnerExt = Cast;
      /// (u8){(u16)Start,+,Step} == {(u8)Start,+,Step}
      if (Cast->getSCEVType() == SCEVTypes::scTruncate) {
        CounterTy = S->getType();
      }
    }
    return matchInnerAddRec(Cast ? Cast->getOperand() : S);
  }

  /// Match {Start,+,Step}
  bool matchInnerAddRec(const SCEV *S) {
    const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(S);
    if (!AddRec)
      return false;
    Start = AddRec->getStart();
    Step = AddRec->getStepRecurrence(SE);
    L = AddRec->getLoop();
    InnerAddRecFlags = AddRec->getNoWrapFlags();
    CounterTy = Start->getType();
    return LIC.isLoopInvariant(SE, Step, L);
  }

  void reset() {
    L = nullptr;
    Inv2Add = Inv2Mul = Start = Step = OuterExt = nullptr;
    InnerExt = nullptr;
  }

private:
  ScalarEvolution &SE;
  LoopInvariantChecker &LIC;
  const Loop *L = nullptr;
  const SCEV *Inv2Add = nullptr;
  const SCEV *Inv2Mul = nullptr;
  const SCEV *Start = nullptr;
  const SCEV *Step = nullptr;
  Type *CounterTy;
  /// Indicates whether the inner addrec is marked as nuw or nsw.
  SCEV::NoWrapFlags InnerAddRecFlags;
  const SCEVZeroExtendExpr *OuterExt = nullptr;
  const SCEVIntegralCastExpr *InnerExt = nullptr;
};

class XsanSCEVExpander : public SCEVExpander {
public:
  explicit XsanSCEVExpander(ScalarEvolution &se, const DataLayout &DL,
                            const char *name, bool PreserveLCSSA = true)
#if LLVM_VERSION_MAJOR <= 15
      : SCEVExpander(se, DL, name, PreserveLCSSA)
#elif LLVM_VERSION_MAJOR >= 22
      : SCEVExpander(se, name, PreserveLCSSA)
#else
      static_assert(false && "Unsupport LLVM version");
#endif
  {
  }
  Value *expandCodeFor(const SCEV *SH, Type *Ty, Instruction *I) {
    Instruction *LastI = I->getPrevNode();
    Value *V = SCEVExpander::expandCodeFor(SH, Ty, I);
    Instruction *Beg = LastI ? LastI->getNextNode()
                             : __xsan::compat::getFirstNonPHIIt(I->getParent());
    Instruction *End = I;
    while (Beg != End) {
      Beg->setMetadata(LLVMContext::MD_nosanitize,
                       llvm::MDNode::get(I->getContext(), {}));
      Beg = Beg->getNextNode();
    }
    return V;
  }
};

/// Compared with `llvm::SplitEdge`, this function merges identical edges
BasicBlock *splitKnownCriticalEdge(BasicBlock *From, BasicBlock *To,
                                   DominatorTree *DT, PostDominatorTree *PDT,
                                   LoopInfo *LI, MemorySSAUpdater *MSSAU,
                                   const Twine &BBName) {
  unsigned SuccNum = GetSuccessorNumber(From, To);
  Instruction *LatchTerm = From->getTerminator();

  CriticalEdgeSplittingOptions Options =
      CriticalEdgeSplittingOptions(DT, LI, MSSAU, PDT)
          .setMergeIdenticalEdges()
          .setPreserveLCSSA();
  BasicBlock *SplitBB =
      SplitKnownCriticalEdge(LatchTerm, SuccNum, Options, BBName);
  /*
   There is a bug when PreserveLCSSA and MergeIdenticalEdges are both set.
   1. MergeIdenticalEdges option might creare >1 identical edges from TIBB to
      SplitBB.
   2. If (From, To) is (Exiting, Exit) in loop, PreserveLCSSA option will insert
      PHINode in SplitBB, which will consumed by the BB To.
   However, PreserveLCSSA option does not consider the existence of multiple
   identical edges from Exiting to Exit (see BreakCriticalEdges.cpp:291 for
   details), while MergeIdenticalEdges option has taken account of it.

   That is to say, there is a wrong path, considering the following case:
      ```llvm
      exiting:
        switch i8 %cond, label %while.body [
            i8 0, label %exit
            i8 10, label %exit
        ]
      exit:
        phi = phi i32 [%val, %exiting], [%val, %exitinh]
      ```
   1. There are >1 identical edges from Exiting to Exit (e.g., `SwitchInst`)
   2. We want to split Exiting->Exit in a loop and use
   `llvm::SplitKnownCriticalEdge`
   3. Options `PreserveLCSSA` and `MergeIdenticalEdges` are both set.
   4. `MergeIdenticalEdges` merges the >1 identical edges from TIBB to SplitBB,
       so far so good as follows:
      ```llvm
      exiting:
        switch i8 %cond, label %while.body [
            i8 0, label %split
            i8 10, label %split
        ]
      split: ; this is the new exit block
        br label %exit
      exit:  ; it is not a exit block any more
        phi = phi i32 [%val, %split]
      ```
    5. However, `PreserveLCSSA` inserts a wrong PHINode in SplitBB, missing some
      predecessors, due to its loss of consideration of multiple identical edges
      from Exiting to Exit, as follows:
      ```llvm
      exiting:
        switch i8 %cond, label %while.body [
            i8 0, label %split
            i8 10, label %split
        ]
      split: ; this is the new exit block
        ; the correct phi should be
        ; lcssa = phi i32 [%val, %exiting], [%val, %exiting]
        lcssa = phi i32 [%val, %exiting]
        br label %exit
      exit:  ; it is not a exit block any more
        phi = phi i32 [%lcssa, %split]
      ```
  */

  /// Here we temporarily fix the bug described above.
  /// TODO: give a PR to the upstream LLVM to fix this bug.
  unsigned Identities = count(predecessors(SplitBB), From);

  for (PHINode &PN : SplitBB->phis()) {
    int Idx = PN.getBasicBlockIndex(From);
    assert(Idx >= 0 && "Invalid Block Index");
    Value *V = PN.getIncomingValue(Idx);

    unsigned CurIdentities = count(PN.blocks(), From);
    assert(CurIdentities <= Identities && "Invalid PHINode");

    for (unsigned i = 0; i < Identities - CurIdentities; ++i) {
      PN.addIncoming(V, From);
    }
  }

  return SplitBB;
}

/// @brief Augment functionality of @llvm::SplitBlockAndInsertIfThen with auto
/// MemoryPhi updates.
Instruction *SplitBlockAndInsertIfThen(Value *Cond, Instruction *SplitBefore,
                                       bool Unreachable,
                                       MDNode *BranchWeights = nullptr,
                                       DominatorTree *DT = nullptr,
                                       LoopInfo *LI = nullptr,
                                       MemorySSAUpdater *MSSAU = nullptr,
                                       BasicBlock *ThenBlock = nullptr) {
  /*
  OldBB:
    ...
    SplitBefore
  */
  BasicBlock *OldBB = SplitBefore->getParent();
  std::optional<DomTreeUpdater> DTU;
  if (DT)
    DTU.emplace(*DT, DomTreeUpdater::UpdateStrategy::Lazy);
  Instruction *Term = llvm::SplitBlockAndInsertIfThen(
      Cond, SplitBefore, Unreachable, BranchWeights, DTU ? &*DTU : nullptr, LI,
      ThenBlock);
  /*
  OldBB:
    ...
    if (Cond) {
      Term
    }
  NewBB:
    SplitBefore
  */
  BasicBlock *NewBB = SplitBefore->getParent();

  // Move MemoryAccesses still tracked in Old, but part of New now.
  // Update accesses in successor blocks accordingly.
  // MemoryPhi((OldBB, ... ), ...) -> MemoryPhi((NewBB, ... ), ...)
  if (MSSAU)
    MSSAU->moveAllAfterSpliceBlocks(OldBB, NewBB, SplitBefore);
  return Term;
}
} // namespace

namespace __xsan {
LoopInvariantChecker::LoopInvariantChecker(const DominatorTree &DT,
                                           AliasAnalysis &AA)
    : DT(DT), AA(AA) {
  const Module &M = *DT.getRoot()->getModule();
  UBSanExists = any_of(M.getFunctionList(), [&](const Function &F) {
    return F.isDeclaration() &&
           compat::startsWith(F.getName(), "__ubsan_handle");
  });
}

/// Due to the UBSan's unmodeled functions, L.isLoopInvariant(...)
/// is not correct. However, as we only take care those simple loops,
/// which contains no user-defined functions.
/// We can simply consider those Inst with loop-invariant operands as
/// loop-invariant, which is what L.hasLoopInvariantOperands(...) does.
bool LoopInvariantChecker::isLoopInvariant(const SCEV *S, const Loop *L) {
  switch (S->getSCEVType()) {
  case scConstant:
    return true;
#if LLVM_VERSION_MAJOR > 22
  case scVScale:
    return false;
  case scPtrToAddr:
#endif
  case scPtrToInt:
  case scTruncate:
  case scZeroExtend:
  case scSignExtend:
    return isLoopInvariant(cast<SCEVCastExpr>(S)->getOperand(), L);
  case scAddRecExpr: {
    const SCEVAddRecExpr *AR = cast<SCEVAddRecExpr>(S);

    // If L is the addrec's loop, it's computable.
    if (AR->getLoop() == L)
      return false;

    // Add recurrences are never invariant in the function-body (null loop).
    if (!L)
      return false;

    // Everything that is not defined at loop entry is variant.
    if (DT.dominates(L->getHeader(), AR->getLoop()->getHeader()))
      return false;
    assert(!L->contains(AR->getLoop()) &&
           "Containing loop's header does not"
           " dominate the contained loop's header?");

    // This recurrence is invariant w.r.t. L if AR's loop contains L.
    if (AR->getLoop()->contains(L))
      return true;

    // This recurrence is variant w.r.t. L if any of its operands
    // are variant.
    for (const auto *Op : AR->operands())
      if (!isLoopInvariant(Op, L))
        return false;

    // Otherwise it's loop-invariant.
    return true;
  }
  case scAddExpr:
  case scMulExpr:
  case scUMaxExpr:
  case scSMaxExpr:
  case scUMinExpr:
  case scSMinExpr:
  case scSequentialUMinExpr: {
    bool HasVarying = false;
    for (const auto *Op : cast<SCEVNAryExpr>(S)->operands()) {
      if (!isLoopInvariant(Op, L))
        return false;
    }
    return true;
  }
  case scUDivExpr: {
    const SCEVUDivExpr *UDiv = cast<SCEVUDivExpr>(S);
    bool LD = isLoopInvariant(UDiv->getLHS(), L);
    if (LD == false)
      return false;
    bool RD = isLoopInvariant(UDiv->getRHS(), L);
    if (RD == false)
      return false;
    return LD && RD;
  }
  case scUnknown:
    // All non-instruction values are loop invariant.  All instructions are loop
    // invariant if they are not contained in the specified loop.
    // Instructions are never considered invariant in the function body
    // (null loop) because they are defined within the "loop".
    return isLoopInvariant(cast<SCEVUnknown>(S)->getValue(), L);
  case scCouldNotCompute:
    llvm_unreachable("Attempt to use a SCEVCouldNotCompute object!");
  default:
    return false;
  }
  llvm_unreachable("Unknown SCEV kind!");
}

bool LoopInvariantChecker::isLoopInvariant(ScalarEvolution &SE, const SCEV *S,
                                           const Loop *L) {
  return UBSanExists ? isLoopInvariant(S, L) : SE.isLoopInvariant(S, L);
}

bool LoopInvariantChecker::isLoopInvariant(Value *V, const Loop *L) {
  bool NotInLoop = L->isLoopInvariant(V);
  if (!UBSanExists || NotInLoop) {
    return NotInLoop;
  }

  auto *I = dyn_cast<Instruction>(V);
  if (!I) {
    return true;
  }
  bool IsPhiNode = isa<PHINode>(I);
  if (IsPhiNode) {
    return false;
  }

  /// If in the loop, it is a loop-invariant instruction only if
  ///   1) I is not a PHI node
  ///   2) All operands are loop-invariant
  if (!L->hasLoopInvariantOperands(I)) {
    return false;
  }

  if (LoadInst *Load = dyn_cast<LoadInst>(I)) {
    return isLoadLoopInvariant(Load, L);
  }

  return true;
}

/*
We cannot use the unified method (i.e., Intrinsic::invariant_start) to judge the
invariance of load instructions just like what LICM does (the relevant code is
in `isLoadInvariantInLoop` @ LICM.cpp).

1. Reason 1: O0 optimization does not insert the invariant.start intrinsic.
2. Reason 2: MSSA is incorrectly affected by calls to UBSan's functions.

Therefore, we just analyze the relevant store instruction to determine if it is
possible to write to the memory location of the load instruction.

CallInst is ignored, because we assume that the Loop is a simple loop without
any calls that may write to memory.
*/
bool LoopInvariantChecker::isLoadLoopInvariant(const LoadInst *LI,
                                               const Loop *L) {
  // 1. If the load is volatile or atomic, it cannot be considered a loop
  // invariant
  if (LI->isVolatile() || LI->isAtomic())
    return false;

  // 2. Check if the pointer operand of the load is invariant within the loop
  if (!L->hasLoopInvariantOperands(LI))
    return false;

  // 3. Traverse all basic blocks within the loop to ensure no store can modify
  // the memory location of the load
  SmallVectorImpl<MemoryLocation> &StoreLocs = getStoresInLoopLazily(L);
  if (StoreLocs.empty()) {
    return true;
  }

  MemoryLocation LoadLoc = MemoryLocation::get(LI);
  return all_of(StoreLocs, [&](auto &StoreLoc) {
    return AA.alias(LoadLoc, StoreLoc) == AliasResult::NoAlias;
  });
}

SmallVectorImpl<MemoryLocation> &
LoopInvariantChecker::getStoresInLoopLazily(const Loop *L) {
  auto It = StoresInLoop.find(L);
  if (It != StoresInLoop.end()) {
    return It->second;
  }
  SmallVector<MemoryLocation, 8> Stores;
  for (BasicBlock *BB : L->getBlocks()) {
    for (Instruction &I : *BB) {
      // Ignore the load instruction itself
      if (!I.mayWriteToMemory())
        continue;

      // If a store instruction is encountered, check if it can write to the
      // same memory location as the load
      if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
        MemoryLocation StoreLoc = MemoryLocation::get(SI);
        Stores.push_back(StoreLoc);
      }
    }
  }
  StoresInLoop[L] = std::move(Stores);
  return StoresInLoop[L];
}

/*
The following pattern is considered as a loop counting pattern.

header:
  %counter = phi [0, %preheader], [%inc, %latch]
  ...
latch:
  %inc = add %counter, 1

Only those PHINode with bitwidth >= target && bitwidth >= 32 are considered.
 */
static bool isLoopCountingPhi(IntegerType *TargetTy, PHINode *PN,
                              BasicBlock *Predecessor, BasicBlock *Latch,
                              bool &ZeroBase) {
  // Check if there are exactly two incoming edges
  if (PN->getNumIncomingValues() != 2)
    return false;

  // Check if the type is integer
  auto *IntTy = dyn_cast<IntegerType>(PN->getType());
  if (!IntTy)
    return false;
  unsigned BitWidth = IntTy->getBitWidth();
  if (BitWidth < TargetTy->getBitWidth() || BitWidth < 32)
    return false;

  bool HasZeroOrOne = false;
  bool HasAddUp = false;
  // Traverse all incoming values of the PHI node
  for (unsigned Idx = 0; Idx < PN->getNumIncomingValues(); Idx++) {
    Value *Incoming = PN->getIncomingValue(Idx);
    BasicBlock *BB = PN->getIncomingBlock(Idx);

    // If incoming is a constant, check if it is 0
    if (ConstantInt *CI = dyn_cast<ConstantInt>(Incoming)) {
      if ((CI->isOne() || CI->isZero()) && BB == Predecessor) {
        HasZeroOrOne = true;
        ZeroBase = CI->isZero();
        continue;
      }
    } else if (AddOperator *IncomingInst = dyn_cast<AddOperator>(Incoming)) {
      // Otherwise, if incoming comes from an addition instruction, check if the
      // addition instruction is %phi + 1 Check if one operand is the current
      // PHI node and the other operand is the constant 1
      Value *Op0 = IncomingInst->getOperand(0);
      Value *Op1 = IncomingInst->getOperand(1);
      if ((Op0 == PN && isa<ConstantInt>(Op1) &&
           cast<ConstantInt>(Op1)->equalsInt(1)) ||
          (Op1 == PN && isa<ConstantInt>(Op0) &&
           cast<ConstantInt>(Op0)->equalsInt(1))) {
        HasAddUp = (BB == Latch);
      }
    }
  }
  return HasZeroOrOne && HasAddUp;
}

/*
 Currently, we only focus on simple loops:
 1. Single header, single predecessor, single latch
 2. Prefer to reuse the existing loop counter (PHI node) instead of creating a
    new one.
*/
static Value *getOrInsertLoopCounter(Loop *Loop, InstrumentationIRBuilder &IRB,
                                     const bool BeforeExiting,
                                     IntegerType *CounterTy = nullptr) {
  BasicBlock *Header = Loop->getHeader(),
             *Predecessor = Loop->getLoopPredecessor(),
             *Latch = Loop->getLoopLatch();

  if (!Header || !Predecessor || !Latch)
    return nullptr; // If the loop does not have a header, it is unsafe to
                    // instrument

  if (CounterTy == nullptr) {
    /// Default type is i64
    CounterTy = IRB.getInt64Ty();
  }

  // If the header has > 2 predecessors, it is unsafe to instrument
  if (!Header->hasNPredecessors(2))
    return nullptr;

  Value *Counter = nullptr;
  /* 1. Try to find an existing loop counter in the loop header (header) */
  for (Instruction &I : *Header) {
    // Only consider PHI nodes
    PHINode *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      continue;

    bool ZeroBase = false;

    if (!isLoopCountingPhi(CounterTy, Phi, Predecessor, Latch, ZeroBase)) {
      continue;
    }
    Counter = Phi;

    /// `isLoopCountingPhi` ensures that bitwidth >= 32, so we do not need to
    /// consider the overflow of the counter.
    if (ZeroBase && BeforeExiting) {
      // If MOP is before the exiting block, we should count it from 1.
      Counter = IRB.CreateAdd(Counter, ConstantInt::get(Counter->getType(), 1),
                              "add_one", true, true);
    } else if (!ZeroBase && !BeforeExiting) {
      // If MOP is after the exiting block, we should count it from 0.
      Counter = IRB.CreateSub(Counter, ConstantInt::get(Counter->getType(), 1),
                              "sub_one", true, true);
    }

    return Counter;
  }

  /* 2. If no existing counter was found, construct a new counter */

  // Create a new PHI node before the first non-PHI insertion point in the
  // header
  auto *OrigInsertPt = &*IRB.GetInsertPoint();
  IRB.SetInsertPoint(&*Header->getFirstInsertionPt());

  LLVMContext &Ctx = Header->getContext();

  // Preset number of incomings: 1 from preheader, plus one for each latch block
  SmallVector<BasicBlock *, 4> Latches;
  Loop->getLoopLatches(Latches);
  unsigned NumIncoming = 1 + Latches.size();
  PHINode *CounterPhi = IRB.CreatePHI(CounterTy, NumIncoming, "loop.count");

  // Incoming value from preheader is
  CounterPhi->addIncoming(ConstantInt::get(CounterTy, (BeforeExiting ? 1 : 0)),
                          Predecessor);
  // For each latch block, insert an addition instruction at its exit and add
  // the result to the PHI node
  for (BasicBlock *Latch : Latches) {
    IRB.SetInsertPoint(Latch->getTerminator());
    // Generate: %inc = add counterPhi, 1
    // Sat Add, to avoid overflow, e.g., 0xff + 1 = 0xff
    Value *Incr = IRB.CreateBinaryIntrinsic(Intrinsic::uadd_sat, CounterPhi,
                                            ConstantInt::get(CounterTy, 1));
    CounterPhi->addIncoming(Incr, Latch);
  }

  // Recover the original insertion point
  IRB.SetInsertPoint(OrigInsertPt);
  return CounterPhi;
}

/// SrcTy == TargetTy: no need to truncate
/// SrcTy < TargetTy: invalid
/// SrcTy > TargetTy: saturation truncate, i.e.,
/// target = src >= (TargetTy::max + 1) ? TargetTy::max + 1 : src
static Value *saturationTruncate(Value *SrcVal, IntegerType *TargetTy,
                                 InstrumentationIRBuilder &IRB) {
  auto *SrcTy = dyn_cast<IntegerType>(SrcVal->getType());
  if (!SrcTy)
    return nullptr;
  if (SrcTy == TargetTy)
    return SrcVal;
  if (SrcTy->getBitWidth() < TargetTy->getBitWidth())
    llvm_unreachable("Invalid truncate");
  if (TargetTy->getBitWidth() == 64) {
    llvm_unreachable("Invalid truncate");
  }

  uint64_t MaxIntPlusOne = (uint64_t(1) << TargetTy->getBitWidth());
  auto *TargetMaxPlusOne = ConstantInt::get(SrcTy, MaxIntPlusOne, false);

  auto *TargetVal = IRB.CreateSelect(
      IRB.CreateICmpUGE(SrcVal, TargetMaxPlusOne), TargetMaxPlusOne, SrcVal);

  return TargetVal;
}

/// Transfer to u64 counter, if SrcTy > TargetTy, do saturation truncation
/// i.e., target = (u64)min(targ_ty::max + 1 : src)
static Value *getCounterBoundedByTargetTy(Value *SrcVal, IntegerType *TargetTy,
                                          IntegerType *PtrIntTy,
                                          InstrumentationIRBuilder &IRB) {
  auto *SrcTy = dyn_cast<IntegerType>(SrcVal->getType());
  if (!SrcTy)
    return nullptr;

  Value *Counter =
      (SrcTy == TargetTy) ? SrcVal : saturationTruncate(SrcVal, TargetTy, IRB);
  assert(SrcTy->getBitWidth() <= 64 && "Unsupported counter type");
  if (SrcTy->getBitWidth() < PtrIntTy->getBitWidth()) {
    Counter = IRB.CreateZExt(Counter, PtrIntTy);
  }

  return Counter;
}

/// Calculate End based on End = Beg + Step * Counter
/// Note: If MOP dominates Exiting (see @arg BeforeExiting), then Counter is
/// the number of backedge taken counts + 1, otherwise it is the number of
/// backedge taken counts
/// Note that, if Step is a negative constant, this function will adjust the
/// access range accordingly as follows (see handleRangeWithNegativeStep):
//  [Beg', End') = [End + |Step|, Beg + |Step|), i.e.,
/// [Start + Step * LoopCount + |Step|, Start + |Step|)
/// e.g., reads 4 bytes from 0x10, then ends at 0x04 with step -4
///   --> Step = -4, Start = 0x10, End = 0x04, LoopCount = 2
///   --> Beg  = 0x10 + (-4) * 2 + |-4| = 0x08
///       End  = 0x10 + |-4| = 0x14
///   --> We obtain the real access range [0x10, 0x14)
/// What's more, if Step is a negative variable, this function just expands
/// Beg and End as usual, and the relevant processing is delegated to the
/// caller or the runtime library.
static bool expandBegAndEnd(Loop *Loop, ScalarEvolution &SE,
                            XsanSCEVExpander &Expander, const SCEV *Start,
                            const SCEV *Step, IntegerType *CounterTy,
                            bool BeforeExiting, InstrumentationIRBuilder &IRB,
                            Value *&Beg, Value *&End, Value *&StepVal) {
  // 1. Try to find an existing loop counter in the loop header (header)
  BasicBlock *Header = Loop->getHeader(),
             *Predecessor = Loop->getLoopPredecessor(),
             *Latch = Loop->getLoopLatch(), *Exit = Loop->getUniqueExitBlock();

  if (!Header || !Predecessor || !Latch)
    return false; // If the loop does not have a header, it is unsafe to
                  // instrument

  // Instruction *InsertPt = Predecessor->getTerminator();
  Instruction *InsertPt = &*Exit->getFirstInsertionPt();

  if (!Expander.isSafeToExpandAt(Start, InsertPt)) {
    // Unsafe to expand StartSCEV at header terminator, skip.
    return false;
  }

  const SCEVConstant *ConstStep = dyn_cast<SCEVConstant>(Step);
  ConstantInt *ConstStepVal = ConstStep ? ConstStep->getValue() : nullptr;
  bool IsConstStepNegative = ConstStepVal && ConstStepVal->isNegative();

  /// If Step is negative,  Start += |Step|, and the following
  /// End = Start + Step * LoopCount will be also added by |Step|
  if (IsConstStepNegative) {
    const auto *PosStep = SE.getNegativeSCEV(Step);
    Start = SE.getAddExpr(Start, PosStep);
  }

  IntegerType *PtrIntTy = getIntTy(Start->getType(), SE.getDataLayout());
  if (!PtrIntTy) {
    llvm_unreachable("Invalid pointer type");
  }

  /// TODO: figure out if it is Okay to insert code in Predecessor block
  Beg = Expander.expandCodeFor(Start, IRB.getPtrTy(), InsertPt);
  StepVal = Expander.expandCodeFor(Step, PtrIntTy, InsertPt);

  /// Does not consider step -1, which should be used as factor of term,
  /// negating the sign of offset.
  bool StepIsOne = ConstStepVal && ConstStepVal->isOne();

  const auto *BackedgeTakenCount = SE.getBackedgeTakenCount(Loop);
  if (!isa<SCEVCouldNotCompute>(BackedgeTakenCount)) {
    /* Try to get the loop's backedge taken count (loop invariant) via SCEV */

    // Note: backedge taken count does not include the first entry into the
    // header, so the actual trip count = backedge taken count + 1 Here we use
    // XsanSCEVExpander to convert the SCEV expression to IR code.

    // Note: XsanSCEVExpander requires DataLayout (DL) and a suitable insertion
    // point, such as using the loop preheader's terminal instruction
    // Theoretically, preheader is better, but a loop predecessor can also
    // work.
    IntegerType *OrigCounterTy =
        cast<IntegerType>(BackedgeTakenCount->getType());
    // CountTy --> PointerIntTy
    BackedgeTakenCount =
        SE.getTruncateOrZeroExtend(BackedgeTakenCount, PtrIntTy);
    const auto *IterCounter =
        BeforeExiting ? SE.getAddExpr(BackedgeTakenCount, SE.getOne(PtrIntTy))
                      : BackedgeTakenCount;

    // If counter type is smaller than the original counter type, we need to
    // truncate the counter to the smaller type.
    if (OrigCounterTy->getBitWidth() > CounterTy->getBitWidth()) {
      assert(PtrIntTy->getBitWidth() >= OrigCounterTy->getBitWidth() &&
             "Invalid truncate");
      APInt MaxIntPlusOne(PtrIntTy->getBitWidth(),
                          (uint64_t(1) << CounterTy->getBitWidth()), false);
      IterCounter = SE.getUMinExpr(IterCounter, SE.getConstant(MaxIntPlusOne));
    }

    const auto *Offset =
        (StepIsOne ? IterCounter : SE.getMulExpr(IterCounter, Step));
    const auto *EndSE = SE.getAddExpr(Start, Offset);

    End = Expander.expandCodeFor(EndSE, IRB.getPtrTy(), InsertPt);

  } else {
    /* If the loop count isn't an invariant, manually insert a loop counter */
    Value *Counter =
        getOrInsertLoopCounter(Loop, IRB, BeforeExiting, CounterTy);
    if (!Counter) {
      return false;
    }
    // Transfer to u64 counter, if SrcTy > TargetTy, do saturation truncation.
    Counter = getCounterBoundedByTargetTy(Counter, CounterTy, PtrIntTy, IRB);

    /// If Step is negative, Beg = End + |Step| = Start + Step * (LoopCount - 1)
    Value *Offset = StepIsOne ? Counter : IRB.CreateMul(Counter, StepVal);
    End = IRB.CreateGEP(IRB.getInt8Ty(), Beg, {Offset});
  }

  if (IsConstStepNegative) {
    /// If Step is negative
    /// [Beg', End') = [End + |Step|, Beg + |Step|)
    std::swap(Beg, End);
    /// If Step is constant, we garantuee that it is a positive value at last.
    StepVal = IRB.CreateNeg(StepVal);
  }

  return true;
}

LoopMopInstrumenter LoopMopInstrumenter::create(Function &F,
                                                FunctionAnalysisManager &FAM,
                                                LoopOptLeval OptLevel) {
  LoopMopInstrumenter LMI(F, FAM, OptLevel);
  return LMI;
}

LoopMopInstrumenter::LoopMopInstrumenter(Function &F,
                                         FunctionAnalysisManager &FAM,
                                         LoopOptLeval OptLevel)
    : F(F), FAM(FAM), LI(FAM.getResult<LoopAnalysis>(F)),
      MSSAU(&FAM.getResult<MemorySSAAnalysis>(F).getMSSA()),
      AA(FAM.getResult<AAManager>(F)),
      DT(FAM.getResult<DominatorTreeAnalysis>(F)),
      PDT(FAM.getResult<PostDominatorTreeAnalysis>(F)),
      DL(F.getParent()->getDataLayout()), OptLevel(OptLevel),
      MopCollected(false), LIC(DT, AA) {
  Module &M = *F.getParent();
  LLVMContext &Ctx = M.getContext();
  IRBuilder<> IRB(Ctx);

  AttributeList Attr;
  Attr = Attr.addFnAttribute(Ctx, Attribute::NoUnwind);
  XsanRangeRead =
      M.getOrInsertFunction("__xsan_read_range", Attr, IRB.getVoidTy(),
                            IRB.getPtrTy(), IRB.getPtrTy());
  XsanRangeWrite =
      M.getOrInsertFunction("__xsan_write_range", Attr, IRB.getVoidTy(),
                            IRB.getPtrTy(), IRB.getPtrTy());

  for (size_t i = 0; i < kNumberOfAccessSizes; i++) {
    const unsigned ByteSize = 1U << i;
    std::string ByteSizeStr = utostr(ByteSize);

    XsanPeriodRead[i] = M.getOrInsertFunction(
        "__xsan_period_read" + ByteSizeStr, Attr, IRB.getVoidTy(),
        IRB.getPtrTy(), IRB.getPtrTy(), IRB.getInt64Ty());
    XsanPeriodWrite[i] = M.getOrInsertFunction(
        "__xsan_period_write" + ByteSizeStr, Attr, IRB.getVoidTy(),
        IRB.getPtrTy(), IRB.getPtrTy(), IRB.getInt64Ty());

    XsanRead[i] = M.getOrInsertFunction("__xsan_read" + ByteSizeStr, Attr,
                                        IRB.getVoidTy(), IRB.getPtrTy());
    XsanWrite[i] = M.getOrInsertFunction("__xsan_write" + ByteSizeStr, Attr,
                                         IRB.getVoidTy(), IRB.getPtrTy());
  }
}

void LoopMopInstrumenter::instrument() {
  if (F.isDeclaration() || F.empty())
    return;
  bool LoopChanged = false;
  switch (OptLevel) {
  case LoopOptLeval::CombineToRangeCheck:
    LoopChanged = combinePeriodicChecks(true);
    break;
  case LoopOptLeval::RelocateInvariantChecks:
    relocateInvariantChecks();
    break;
  case LoopOptLeval::CombinePeriodicChecks:
    LoopChanged = combinePeriodicChecks(false);
    break;
  case LoopOptLeval::Full:
    LoopChanged |= combinePeriodicChecks(false);
    // Invariant checks might insert new branches, so we need to run it later
    // than CombinePeriodicChecks.
    LoopChanged |= relocateInvariantChecks();
    break;
  case LoopOptLeval::NoOpt:
    return;
  }
  auto PA = llvm::PreservedAnalyses::all();
  // abandon PDT as we cannot hot-update it in `SplitBlock*`.
  PA.abandon<PostDominatorTreeAnalysis>();
  FAM.invalidate(F, PA);
}
SmallVectorImpl<LoopMopInstrumenter::LoopMop> &
LoopMopInstrumenter::getLoopMopCandidates() {
  if (!MopCollected) {
    collectLoopMopCandidates();
  }
  return LoopMopCandidates;
}

// Do not instrument known MOPs that come from compiler
// instrumentatin. The user has no way of suppressing them.
// This function comes from TSan's ThreadSanitizer.cpp
static bool shouldInstrumentReadWriteFromAddress(const Module *M, Value *Addr) {
  // Peel off GEPs and BitCasts.
  Addr = Addr->stripInBoundsOffsets();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->hasSection()) {
      StringRef SectionName = GV->getSection();
      // Check if the global is in the PGO counters section.
      auto OF = Triple(M->getTargetTriple()).getObjectFormat();
      if (compat::endsWith(
              SectionName,
              getInstrProfSectionName(IPSK_cnts, OF, /*AddSegmentInfo=*/false)))
        return false;
    }

    // Check if the global is private gcov data.
    if (compat::startsWith(GV->getName(), "__llvm_gcov") ||
        compat::startsWith(GV->getName(), "__llvm_gcda"))
      return false;
  }

  // Do not instrument acesses from different address spaces; we cannot deal
  // with them.
  if (Addr) {
    Type *PtrTy = cast<PointerType>(Addr->getType()->getScalarType());
    if (PtrTy->getPointerAddressSpace() != 0)
      return false;
  }

  return true;
}

void LoopMopInstrumenter::collectLoopMopCandidates() {
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  SmallVector<LoopMop, 16> LoopMops;

  // for (auto* L : LI)  only traverse the top-level loops, not nested loops.
  // See LI.getTopLevelLoops() for more details. Therefore, we do not directly
  // iterate the LoopInfo, but just iterate all MOPs and filter out those not
  // in a loop.
  for (BasicBlock &BB : F) {
    Loop *Loop = LI.getLoopFor(&BB);
    if (!Loop) {
      continue;
    }
    if (!isSimpleLoop(Loop)) {
      continue;
    }

    for (Instruction &Inst : BB) {
      if (shouldSkip(Inst))
        continue;
      // Does not consider volatile/atomic/dbginfo instructions
      if (Inst.isVolatile() || Inst.isAtomic() || isa<DbgInfoIntrinsic>(Inst))
        continue;
      /* 1. Filter out non-memory instructions */
      Value *Addr = nullptr;
      size_t MopSize = 0;
      bool InBranch = false;
      bool IsWrite = false;
      if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
        Addr = SI->getPointerOperand();
        MopSize = DL.getTypeStoreSizeInBits(SI->getValueOperand()->getType());
        IsWrite = true;
      } else if (auto *LI = dyn_cast<LoadInst>(&Inst)) {
        Addr = LI->getPointerOperand();
        MopSize = DL.getTypeStoreSizeInBits(LI->getType());
        IsWrite = false;
      } else if (isa<CallBase>(Inst) &&
                 /* TODO: use more precise memory model */
                 Inst.mayWriteToMemory()) {
        filterAndAddMops(LoopMops);
        LoopMops.clear();
        continue;
      }

      if (!Addr ||
          !shouldInstrumentReadWriteFromAddress(Inst.getModule(), Addr))
        continue;

      /* 2. Filter out non-regular memory instructions */
      /// Only consider 8, 16, 32, 64, 128 bit access
      if (MopSize != 8 && MopSize != 16 && MopSize != 32 && MopSize != 64 &&
          MopSize != 128) {
        continue;
      }

      MopSize /= 8;

      auto *LoopLatch = Loop->getLoopLatch();
      assert(LoopLatch && "Loop must have single latch");
      if (!DT.dominates(Inst.getParent(), LoopLatch)) {
        // If Inst does not dominate the loop latch, it's within a loop branch
        InBranch = true;
      }

      /*
        struct LoopMop {
          Instruction *Mop;
          Value *Address;
          Loop *Loop;
          size_t MopSize;
          bool InBranch;
          bool IsWrite;
        };
      */
      LoopMop Candidate = {&Inst, Addr, Loop, MopSize, {}, InBranch, IsWrite};
      LoopMops.push_back(Candidate);
    }
    filterAndAddMops(LoopMops);
    LoopMops.clear();
  }
  MopCollected = true;
}

/// Filter out those obvious duplicate MOPs in the same BB,
/// being formalized as follows
/// For ∀m1 ≠ m2 ∈ MOPs, m1 is a duplicate of m2 if
///     1. m1.addr = m2.addr
///     2. m1.type = m2.type ∨ m1.type = read
///     3. ∀ i ∈ (m1.loc, m2.loc), isNotCall(i)
/// `collectLoopMopCandidates` is the caller of this function,
///  guaranting that the third condition holding.
void LoopMopInstrumenter::filterAndAddMops(SmallVectorImpl<LoopMop> &MOPs) {
  if (MOPs.empty())
    return;
  if (MOPs.size() == 1) {
    LoopMopCandidates.push_back(std::move(MOPs.pop_back_val()));
    return;
  }
  SmallMapVector<Value *, DupVec *, 4> SeenAddrs;
  SmallVector<LoopMop, 8> MopsReversed;
  /// Reserve enough space to make the space stable,
  /// avoiding pointer to MopsReversed.back().DupTo to be invalidated.
  MopsReversed.reserve(MOPs.size());
  /// read-after-write MOPs do NOT exist due to the compilation optimization
  /// Therefore, we traverse the MOPs in reverse order to retrieve the write
  /// MOPs first if there are any read-before-write MOPs.
  for (LoopMop &Mop : reverse(MOPs)) {
    auto &[Inst, Addr, Loop, MopSize, DupTo, InBranch, IsWrite] = Mop;
    auto *It = SeenAddrs.find(Addr);
    if (It != SeenAddrs.end()) {
      // If there are other MOPs with the same address, check if they are
      // duplicates
      It->second->push_back(Inst);
    } else {
      MopsReversed.push_back(std::move(Mop));
      // &MopsReversed.back().DupTo is guauranteed to be stable
      // because MopsReversed.reserve(MOPs.size()) is called.
      // Therefore, this pointer is valid forever.
      SeenAddrs[Addr] = &MopsReversed.back().DupTo;
    }
  }
  // Move the MOPs back to the original order
  LoopMopCandidates.append(std::make_move_iterator(MopsReversed.rbegin()),
                           std::make_move_iterator(MopsReversed.rend()));
}

bool LoopMopInstrumenter::isSimpleLoop(const Loop *Loop) {
  if (!Loop)
    return false;
  if (SimpleLoops.contains(Loop))
    return true;
  if (ComplexLoops.contains(Loop))
    return false;

  /// TODO: relax this condition
  /// ONE latch, ONE predecessor, ONE header, ONE exiting block, ONE exit block
  /// Note that, some Loop may have > 1 exit, but only 1 uniq exit, hence we
  /// use API `getUniqueExitBlock` to get the unique exit block.
  /// i.e., switch ... %latch { ..., %exit; ..., %exit; } has 2 exits, but only
  /// 1 uniq exit.
  if (!Loop->getHeader() || !Loop->getLoopPredecessor() ||
      !Loop->getLoopLatch() || !Loop->getUniqueExitBlock() ||
      !Loop->getExitingBlock()) {
    ComplexLoops.insert(Loop);
    return false;
  }

  // Loop that contains atomic instructions or calls is not simple.
  // Notaly, a loop with a pure function call that does not write to
  // memory is also considered as a simple loop.
  for (const BasicBlock *BB : Loop->getBlocks()) {
    for (const Instruction &I : *BB) {
      if (shouldSkip(I))
        continue;
      if (!I.mayWriteToMemory())
        continue;
      if (isa<DbgInfoIntrinsic>(I))
        continue;
      /// TODO: should consider Atomic for ASan ?
      if (I.isAtomic() || isa<CallBase>(&I)) {
        ComplexLoops.insert(Loop);
        return false;
      }
    }
  }
  SimpleLoops.insert(Loop);
  return true;
}

/*
   while (cond) {
      CHECK(ptr)
      *ptr++ = val;
   }

-->

   ptr_init = ptr;
   loop_count = 0;
   while (cond) {
      *ptr++ = val;
      loop_count++;
   }
   CHECK(ptr_init, ptr_init + loop_count * sizeof(val))

Currently only targets the simplest loops, i.e.:
 1. Single header, single predecessor, single latch, single exit
 2. Loads/stores not within branches.
 3. Canonical loops, i.e., well-formed loops.
TODO: Perhaps we can relax these conditions in the future. For example, multiple
exits, multiple exits, multiple latches (in which case each MOP needs to
maintain a corresponding counter)
 */
bool LoopMopInstrumenter::combinePeriodicChecks(bool RangeAccessOnly) {
  bool LoopChanged = false;
  // Get SCEV analysis result
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);

  XsanSCEVExpander Expander(SE, DL, "expander");

  // Those MOPs in the same BB are guaranteed to be adjacent and ordered by
  // their IR order.
  for (LoopMop &Mop : getLoopMopCandidates()) {
    auto &[Inst, Addr, _, MopSize, DupTo, InBranch, IsWrite] = Mop;
    if (InBranch) {
      // Periodic MOPs combination is not supported in branches currently, skip.
      continue;
    }

    /* 1. Filter out non-periodic MOPs */
    IntegerType *CounterTy =
        IntegerType::get(F.getContext(), DL.getPointerSizeInBits());
    auto *PtrSCEV = SE.getSCEV(Addr);
    if (!PtrSCEV)
      continue;

    const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PtrSCEV);

    /*
    There are three access models:
    1. Range access: access [L, R)
    2. Periodic access: access ([L, R), AccessSize, Step)
      - Split [L, R) by Step, and access AccessSize each step.
    3. Cyclic access: access ([L, R), Beg, End)
      - Access [Beg, R) and [L, End)
    */

    if (!AR) {
      /// transfer SCEVAddExpr(SCEVAddRecExpr) to SCEVAddRecExpr(SCEVAddExpr)
      auto Rewriter = PtrAddExpr2PtrAddRecRewriter::get(PtrSCEV, SE, LIC);
      if (!Rewriter.has_value()) {
        continue;
      }
      auto AddRec = Rewriter->assembleNewAddRec();
      if (!AddRec) {
        continue;
      }
      AR = *AddRec;
      IntegerType *NewCounterTy = Rewriter->getCounterTy();
      if (NewCounterTy->getBitWidth() < CounterTy->getBitWidth()) {
        /// counter bitwidth < pointer bitwidth iff this is a cyclic access
        /// We now only support those simple cases with Beg = L
        const auto *L = Rewriter->getBasePtr();
        /// Those cyclic accesses with ZERO offset are simple accesses.
        if (!Rewriter->getBaseOffset()->isZero()) {
          continue;
        }
        /// TODO: support cyclic access.
        /// TODO: support resolve of NoWrapFlags.
        // We can assume that the GEP does not overflow.
      }
      CounterTy = NewCounterTy;
    }

    if (!AR)
      continue;

    if (!AR->isAffine()) {
      // If AR is not in form A + B * x or A/B is not a loop-invariant,
      // skip.
      continue;
    }

    /// TODO: support N-op case AddRec {S_{N-1},+,S_{N-2},+,...,+,S_0}
    /// e.g., {A, + , B, + , C} = A + x (B + C x) = A + x B + x^2 C
    /// AR.isQuadratic() is true if A/B/C are loop-invariants.

    const auto *Step = AR->getStepRecurrence(SE);

    /// NOTE that, the direct parent LOOP of a AddRec MOP is NOT necessarily the
    /// relevant LOOP of the AddRec, i.e., the MOP address is an invariant of
    /// the direct parent LOOP, but changes with the outer loops.
    Loop *L = const_cast<Loop *>(AR->getLoop());
    if (!isSimpleLoop(L)) {
      continue;
    }

    // ----------- Extract Loop-Invariant step -----------
    const SCEVConstant *ConstStep = dyn_cast<SCEVConstant>(Step);
    bool IsRangeAccess;
    if (!ConstStep) {
      // Step must be a loop invariant, guaranteed by the above AR->isAffine()
      // If Step is not a constant, do not use range access to model.
      IsRangeAccess = false;
    } else {
      ConstantInt *StepVal = ConstStep->getValue();
      // Negative step is considered as positive step
      size_t StepValInt = StepVal->getSExtValue() < 0 ? -StepVal->getSExtValue()
                                                      : StepVal->getSExtValue();
      IsRangeAccess = (StepValInt == MopSize);
    }

    if (RangeAccessOnly && !IsRangeAccess) {
      // If step is not MopSize, it's not full range access, skip
      continue;
    }

    auto *ExitBlock = L->getUniqueExitBlock();
    auto *Exiting = L->getExitingBlock();

    /* 2. Combine periodic MOPs into a single range check */

    // If dominates Exiting, need to add 1 to the counter, otherwise no need
    // to add
    bool IsMopBeforeExiting = DT.dominates(Inst->getParent(), Exiting);

    // The only predecessor of exit should be the exiting block.
    if (ExitBlock->getUniquePredecessor() != Exiting) {
      // ExitBlock has > 1 uniq predecessors, edge Exiting->ExitBlock must be
      // a critical edge, which requires |succ(pred)| > 1 or |pred(succ)| > 1.

      // Update ExitBlock
      // This optimization splits block without update PDT, causing PDT cannot
      // be updated correctly in the following splitKnownCriticalEdge.
      // Hence, we set PDT to nullptr here.
      ExitBlock = splitKnownCriticalEdge(Exiting, ExitBlock, &DT, nullptr, &LI,
                                         &MSSAU, "xsan.loop.exit");
      LoopChanged = true;
    }

    InstrumentationIRBuilder IRB(&*ExitBlock->getFirstInsertionPt());

    const auto *Start = AR->getStart();
    Value *Beg, *End, *StepVal;
    if (!expandBegAndEnd(L, SE, Expander, Start, Step, CounterTy,
                         IsMopBeforeExiting, IRB, Beg, End, StepVal)) {
      // If failed to expand, skip.
      continue;
    }

    if (IsRangeAccess) {
      // void __xsan_read_range(const void *beg, const void *end) {
      // void __xsan_write_range(const void *beg, const void *end) {
      CallInst *Check =
          IRB.CreateCall(IsWrite ? XsanRangeWrite : XsanRangeRead, {Beg, End});
      Check->setDebugLoc(Mop.Mop->getDebugLoc());
    } else {
      size_t Idx = compat::countr_zero(MopSize);
      // __xsan_period_readX(const void *beg, const void *end, size_t step)
      // __xsan_period_writeX(const void *beg, const void *end, size_t step)
      CallInst *Check =
          IRB.CreateCall(IsWrite ? XsanPeriodWrite[Idx] : XsanPeriodRead[Idx],
                         {Beg, End, StepVal});
      Check->setDebugLoc(Mop.Mop->getDebugLoc());
    }

    NumPeriodChecksCombinedDup += tagMopAsDelegated(Mop);
    NumPeriodChecksCombined++;
  }

  if (options::ClDebug) {
    Log.setFunction(F.getName());
    Log.addLog("[LoopOpt] #{Combined Loop Mops}", NumPeriodChecksCombined);
    Log.addLog("[LoopOpt] #{Dup Comb. Reduction}", NumPeriodChecksCombinedDup);
  }

  return LoopChanged;
}

// Instrument a flag variable to indicate if the MOP is executed in loop.
// Returns the new insert point pointing to the new branch.
// --- Before instrumentation ---
// Exit:
//   ...
//
// --- After instrumentation ---
// Exit:
//   indicator = ...; // new instruction
//   if (indicator) {
//     --> insert here
//   }
static Instruction *instrumentIndicator(Instruction *Inst,
                                        BasicBlock *ExitBlock,
                                        DominatorTree &DT, LoopInfo &LI,
                                        MemorySSAUpdater &MSSAU) {
  /// TODO: do not rely on the optimization, just instrument a GOOD IR with
  /// PHINode.

  /* Insert code to get the indicator indicating if the MOP is executed in loop
   */

  Function *F = Inst->getParent()->getParent();
  BasicBlock &Entry = F->getEntryBlock();

  InstrumentationIRBuilder IRB(&*Entry.getFirstInsertionPt());
  auto *IndicatorAlloc =
      IRB.CreateAlloca(IRB.getInt1Ty(), nullptr, "indicator");
  /// Default to false, indicating that MOP is not executed.
  IRB.CreateStore(IRB.getFalse(), IndicatorAlloc);

  /// Set the indicator to true just before the MOP.
  IRB.SetInsertPoint(Inst);
  IRB.CreateStore(IRB.getTrue(), IndicatorAlloc);

  IRB.SetInsertPoint(&*ExitBlock->getFirstInsertionPt());
  auto *Indicator = IRB.CreateLoad(IRB.getInt1Ty(), IndicatorAlloc);

  /* Insert new branch according to the value of indicator */
  Instruction *EqTrue =
      (Instruction *)IRB.CreateICmpEQ(Indicator, IRB.getTrue());
  /*
    If DT (DominatorTree) is used to be updated in other places, we cannot use
    DTU (DomTreeUpdater) here, because we need to consider the consistence with
    the DT. As if DTU is used, the new DT should be obtained by
    DTU.getDomTree().
  */
  auto *Term = ::SplitBlockAndInsertIfThen(EqTrue, EqTrue->getNextNode(), false,
                                           nullptr, &DT, &LI, &MSSAU);

  return Term;
}

bool LoopMopInstrumenter::relocateInvariantChecks() {
  bool LoopChanged = false;
  Instruction *LastInertPt = nullptr;
  BasicBlock *LastBB = nullptr;
  for (LoopMop &Mop : getLoopMopCandidates()) {
    auto &[Inst, Addr, L, MopSize, DupTo, InBranch, IsWrite] = Mop;
    if (!LIC.isLoopInvariant(Addr, L)) {
      // Skip if Addr is not a loop invariant
      continue;
    }
    // Top loop to maintain the invarianty of Addr
    Loop *TopL = L, *ParentL = L->getParentLoop();
    while (ParentL && LIC.isLoopInvariant(Addr, ParentL) &&
           /// TODO: for ASan, simple loop is not necessary, we need to relax
           /// this condition
           isSimpleLoop(ParentL)) {
      TopL = ParentL;
      ParentL = TopL->getParentLoop();
    }

    auto *Preheader = TopL->getLoopPreheader();
    // We should alse consider the branch from preheader to MOP if InBranch is
    // false.
    bool IsInBranch =
        InBranch ? InBranch : !PDT.dominates(Inst->getParent(), Preheader);

    /* Get the insert point for invariant check relocating */
    Instruction *InsertPt;
    bool SameBBWithLast = LastBB && LastBB == Inst->getParent();
    Instruction *AddrInst = dyn_cast<Instruction>(Addr);
    bool HoistToPreheader = !IsInBranch && Preheader;
    if (SameBBWithLast) {
      // Just reuse the last insert point
      InsertPt = LastInertPt;
    } else if (HoistToPreheader) {
      // If 1) preheader exists, 2) MOP is not in branch, and 3) Addr dom
      // preheader's terminator, insert at the terminator of the preheader.
      InsertPt = Preheader->getTerminator();
    } else {
      // If no preheader, sinstrument on the exit.
      auto *ExitBlock = TopL->getUniqueExitBlock();
      auto *Exiting = TopL->getExitingBlock();

      // The only predecessor of exit should be the exiting block.
      if (ExitBlock->getUniquePredecessor() != Exiting) {
        // ExitBlock has > 1 uniq predecessors, edge Exiting->ExitBlock must be
        // a critical edge, which requires |succ(pred)| > 1 or |pred(succ)| > 1.

        // Update ExitBlock
        // This optimization splits block without update PDT, causing PDT cannot
        // be updated correctly in the following splitKnownCriticalEdge.
        // Hence, we set PDT to nullptr here.
        ExitBlock = splitKnownCriticalEdge(Exiting, ExitBlock, &DT, nullptr,
                                           &LI, &MSSAU, "xsan.loop.exit");

        LoopChanged = true;
      }
      if (IsInBranch) {
        // Insert indicator to indicate if the MOP is executed in loop.
        InsertPt = instrumentIndicator(Inst, ExitBlock, DT, LI, MSSAU);
        if (!InsertPt) {
          continue;
        }
      } else {
        // If not in branch, insert at the beginning of the exit block.
        InsertPt = &*ExitBlock->getFirstInsertionPt();
      }
    }

    /// If UBSan enabled, the de facto address invariant may be inside the loop.
    /// For these cases, we need to copy the address into the insert point.
    if (AddrInst && !DT.dominates(AddrInst, InsertPt)) {
      Instruction *ClonedAddr = AddrInst->clone();
#if LLVM_VERSION_MAJOR >= 22
      ClonedAddr->insertBefore(InsertPt->getIterator());
#else
      ClonedAddr->insertBefore(InsertPt);
#endif
      ClonedAddr->setMetadata(LLVMContext::MD_nosanitize,
                              llvm::MDNode::get(AddrInst->getContext(), {}));
      Addr = ClonedAddr;
    }

    size_t Idx = compat::countr_zero(MopSize);

    InstrumentationIRBuilder IRB(InsertPt);

    // __xsan_readX(const void *beg)
    // __xsan_writeX(const void *beg)
    CallInst *Check =
        IRB.CreateCall(IsWrite ? XsanWrite[Idx] : XsanRead[Idx], {Addr});
    Check->setDebugLoc(Mop.Mop->getDebugLoc());
    NumInvChecksRelocatedDup += tagMopAsDelegated(Mop);
    NumInvChecksRelocated++;

    LastInertPt = InsertPt;
    LastBB = Inst->getParent();
  }

  if (options::ClDebug) {
    Log.setFunction(F.getName());
    Log.addLog("[LoopOpt] #{Relocated Loop Mops}", NumInvChecksRelocated);
    Log.addLog("[LoopOpt] #{Dup Reloc. Reduction}", NumInvChecksRelocatedDup);
  }

  return LoopChanged;
}

unsigned LoopMopInstrumenter::tagMopAsDelegated(LoopMop &Mop) {
  Instruction *Inst = Mop.Mop;
  auto &Dup = Mop.DupTo;
  DelegateToXSan::set(*Inst);
  for (Instruction *DupInst : Dup) {
    DelegateToXSan::set(*DupInst);
  }
  return Dup.size();
}

} // namespace __xsan
