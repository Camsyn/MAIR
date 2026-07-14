#include "MAIR/Analysis/LoopAnalysis.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

AnalysisKey LoopAnalysis::Key;

MAIRLoopInfo::MAIRLoopInfo(MAIRGraph &G, FunctionAnalysisManager &Manager)
    : Graph(&G), FAM(&Manager), Func(G.getMutableFunction()) {
  if (!Func)
    return;
  LI = &FAM->getResult<llvm::LoopAnalysis>(*Func);
  DT = &FAM->getResult<DominatorTreeAnalysis>(*Func);
  PDT = &FAM->getResult<PostDominatorTreeAnalysis>(*Func);
}

Loop *MAIRLoopInfo::getLoopFor(const Observation &Obs) const {
  if (!isAvailable())
    return nullptr;
  const BasicBlock *BB = Obs.getBasicBlock();
  return BB ? LI->getLoopFor(BB) : nullptr;
}

bool MAIRLoopInfo::isSimpleLoop(const Loop &L) const {
  if (!L.getHeader() || !L.getLoopPreheader() || !L.getLoopLatch() ||
      !L.getUniqueExitBlock() || !L.getExitingBlock())
    return false;

  // A natural-loop summary is not enough for an observation proof: an
  // irreducible SCC inside the summary can revisit a dominated block more
  // than once in one loop iteration.  Keep all loop transforms conservative
  // until that CFG shape has been structurally rejected.
  LoopBlocksRPO RPOT(const_cast<Loop *>(&L));
  RPOT.perform(LI);
  if (containsIrreducibleCFG<const BasicBlock *>(RPOT, *LI))
    return false;

  for (const BasicBlock *BB : L.getBlocks()) {
    for (const Instruction &I : *BB) {
      if (isa<DbgInfoIntrinsic>(I))
        continue;
      if (I.isAtomic())
        return false;
      if (isa<CallBase>(I) && I.mayWriteToMemory())
        return false;
    }
  }
  return true;
}

bool MAIRLoopInfo::isValueAvailableAt(const Value &V,
                                      const ObservationSite &Site) const {
  if (!isAvailable())
    return false;

  const Instruction *Anchor = nullptr;
  if (Site.isEdge()) {
    const BasicBlock *From = Site.getEdgeFrom();
    if (!From || !From->getTerminator())
      return false;
    Anchor = From->getTerminator();
  } else {
    Anchor = Site.getAnchor();
    if (!Site.getBasicBlock() || !Anchor)
      return false;
  }

  if (isa<Constant>(&V) || isa<Argument>(&V))
    return true;
  const auto *Def = dyn_cast<Instruction>(&V);
  return Def && DT->dominates(Def, Anchor);
}

bool MAIRLoopInfo::executesOncePerRelevantIteration(const Observation &Obs,
                                                    const Loop &L) const {
  if (!isAvailable() || !isSimpleLoop(L) || !Obs.hasAlwaysGuard() ||
      !Obs.getSite().isAnchored())
    return false;

  const BasicBlock *SiteBlock = Obs.getBasicBlock();
  BasicBlock *Latch = L.getLoopLatch();
  if (!SiteBlock || !Latch || LI->getLoopFor(SiteBlock) != &L)
    return false;

  return DT->dominates(SiteBlock, Latch);
}

const SCEV *MAIRLoopInfo::getExactExecutionCountSCEV(const Observation &Obs,
                                                     const Loop &L) const {
  if (!executesOncePerRelevantIteration(Obs, L))
    return nullptr;

  const SCEV *BackedgeTakenCount = getSE().getBackedgeTakenCount(&L);
  if (isa<SCEVCouldNotCompute>(BackedgeTakenCount) ||
      !BackedgeTakenCount->getType()->isIntegerTy())
    return nullptr;

  unsigned Width =
      cast<IntegerType>(BackedgeTakenCount->getType())->getBitWidth();
  IntegerType *WideType =
      IntegerType::get(L.getHeader()->getContext(), Width + 1);
  const SCEV *Count = getSE().getZeroExtendExpr(BackedgeTakenCount, WideType);

  BasicBlock *Exiting = L.getExitingBlock();
  const BasicBlock *SiteBlock = Obs.getBasicBlock();
  if (!Exiting || !SiteBlock)
    return nullptr;
  if (DT->dominates(SiteBlock, Exiting))
    Count = getSE().getAddExpr(Count, getSE().getOne(WideType));
  return Count;
}

bool MAIRLoopInfo::isEveryIterationAccess(const MemoryAccess &Access,
                                          const Loop &L) const {
  if (!isAvailable())
    return false;
  BasicBlock *Latch = L.getLoopLatch();
  if (!Latch)
    return false;
  for (const Instruction *I : Access.getInstructions()) {
    if (!I || !L.contains(I->getParent()))
      return false;
    if (!DT->dominates(I->getParent(), Latch))
      return false;
  }
  return true;
}

bool MAIRLoopInfo::isGuaranteedOnLoopEntry(const MemoryAccess &Access,
                                           const Loop &L) const {
  if (!isAvailable())
    return false;
  BasicBlock *Preheader = L.getLoopPreheader();
  if (!Preheader)
    return false;
  for (const Instruction *I : Access.getInstructions()) {
    if (!I || !L.contains(I->getParent()))
      return false;
    if (!PDT->dominates(I->getParent(), Preheader))
      return false;
  }
  return true;
}

bool MAIRLoopInfo::isLoopInvariant(const Value *V, const Loop &L) const {
  return V && L.isLoopInvariant(V);
}

bool MAIRLoopInfo::isLoopInvariant(const AbstractValue &Value,
                                   const Loop &L) const {
  if (Value.isConstant())
    return true;
  if (Value.isSymbol())
    return isLoopInvariant(Value.getSymbol(), L);
  return false;
}

bool MAIRLoopInfo::isLoopInvariant(const MemoryAccess &Access,
                                   const Loop &L) const {
  const AbstractAccess *Loc = Access.getContiguousRegion();
  return Loc && isLoopInvariant(Loc->getBase(), L) &&
         isLoopInvariant(Loc->getOffset(), L) &&
         isLoopInvariant(Loc->getSize(), L);
}

Loop *MAIRLoopInfo::getTopInvariantLoop(const MemoryAccess &Access,
                                        Loop &Start) const {
  if (!isSimpleLoop(Start) || !isLoopInvariant(Access, Start))
    return nullptr;

  Loop *Top = &Start;
  while (Loop *Parent = Top->getParentLoop()) {
    if (!isSimpleLoop(*Parent) || !isLoopInvariant(Access, *Parent))
      break;
    Top = Parent;
  }
  return Top;
}

Observation *MAIRLoopInfo::getPreheaderExit(const Loop &L) const {
  if (!Graph)
    return nullptr;
  BasicBlock *Preheader = L.getLoopPreheader();
  return Preheader ? Graph->getBBExitRepresentative(*Preheader) : nullptr;
}

bool MAIRLoopInfo::hasDedicatedExit(const Loop &L) const {
  std::optional<MAIRLoopExit> Exit = describeUniqueExit(L);
  return Exit && Exit->IsDedicated;
}

std::optional<MAIRLoopExit>
MAIRLoopInfo::describeUniqueExit(const Loop &L) const {
  BasicBlock *Exit = L.getUniqueExitBlock();
  BasicBlock *Exiting = L.getExitingBlock();
  if (!Exit || !Exiting)
    return std::nullopt;
  return MAIRLoopExit{Exiting, Exit, Exit->getUniquePredecessor() == Exiting};
}

Observation *MAIRLoopInfo::getUniqueExitEntry(const Loop &L) const {
  if (!Graph)
    return nullptr;
  BasicBlock *Exit = L.getUniqueExitBlock();
  return Exit ? Graph->getBBEntryRepresentative(*Exit) : nullptr;
}

ScalarEvolution &MAIRLoopInfo::getSE() const {
  if (!FAM || !Func)
    report_fatal_error("MAIR LoopAnalysis requires a FunctionAnalysisManager");
  return FAM->getResult<ScalarEvolutionAnalysis>(*Func);
}

LoopAnalysis::Result LoopAnalysis::run(MAIRGraph &G,
                                       MAIRGraphAnalysisManager &) {
  if (!G.isLive())
    report_fatal_error("MAIR LoopAnalysis cannot run on a consumed graph");
  if (!FAM)
    report_fatal_error(
        "MAIR LoopAnalysis requires a FunctionAnalysisManager bridge");
  return MAIRLoopInfo(G, *FAM);
}

} // namespace mair
