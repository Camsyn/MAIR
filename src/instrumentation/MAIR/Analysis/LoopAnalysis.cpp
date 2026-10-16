#include "MAIR/Analysis/LoopAnalysis.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

AnalysisKey LoopAnalysis::Key;

MAIRLoopInfo::MAIRLoopInfo(MAIRGraph &G, FunctionAnalysisManager &Manager)
    : Graph(&G), FAM(&Manager), Func(G.getMutableFunction()) {
  if (!Func)
    return;
  LI = &G.getLoopInfo();
  DT = &G.getDominatorTree();
}

Loop *MAIRLoopInfo::getLoopFor(const ProgramPoint &PP) const {
  if (!isAvailable())
    return nullptr;
  const BasicBlock *BB = PP.getBasicBlock();
  return BB ? LI->getLoopFor(BB) : nullptr;
}

bool MAIRLoopInfo::isValueAvailableAt(const Value &V,
                                      const ProgramPointSite &Site) const {
  if (!isAvailable())
    return false;

  const Instruction &Anchor = Site.getAnchor();

  if (isa<Constant>(&V) || isa<Argument>(&V))
    return true;
  const auto *Def = dyn_cast<Instruction>(&V);
  return Def && DT->dominates(Def, &Anchor);
}

bool MAIRLoopInfo::executesOncePerRelevantIteration(const ProgramPoint &PP,
                                                    const Loop &L) const {
  if (!isAvailable() || !PP.hasAlwaysGuard() || !PP.getSite().isAnchored())
    return false;

  // An internal irreducible cycle can revisit a dominated program point more
  // than once during one natural-loop iteration. This check belongs to
  // execution counting; barrier legality is represented separately by graph
  // edges.
  LoopBlocksRPO RPOT(const_cast<Loop *>(&L));
  RPOT.perform(LI);
  if (containsIrreducibleCFG<const BasicBlock *>(RPOT, *LI))
    return false;

  const BasicBlock *SiteBlock = PP.getBasicBlock();
  BasicBlock *Latch = L.getLoopLatch();
  if (!SiteBlock || !Latch || LI->getLoopFor(SiteBlock) != &L)
    return false;

  return DT->dominates(SiteBlock, Latch);
}

const SCEV *MAIRLoopInfo::getExactExecutionCountSCEV(const ProgramPoint &PP,
                                                     const Loop &L) const {
  if (!executesOncePerRelevantIteration(PP, L))
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
  const BasicBlock *SiteBlock = PP.getBasicBlock();
  if (!Exiting || !SiteBlock)
    return nullptr;
  if (DT->dominates(SiteBlock, Exiting))
    Count = getSE().getAddExpr(Count, getSE().getOne(WideType));
  return Count;
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

bool MAIRLoopInfo::isLoopInvariant(const Observation &Obs,
                                   const Loop &L) const {
  const AbstractAccess *Loc = Obs.getContiguousRegion();
  return Loc && isLoopInvariant(Loc->getBase(), L) &&
         isLoopInvariant(Loc->getOffset(), L) &&
         isLoopInvariant(Loc->getSize(), L);
}

Loop *MAIRLoopInfo::getTopInvariantLoop(const Observation &Obs,
                                        Loop &Start) const {
  if (!isLoopInvariant(Obs, Start))
    return nullptr;

  Loop *Top = &Start;
  while (Loop *Parent = Top->getParentLoop()) {
    if (!isLoopInvariant(Obs, *Parent))
      break;
    Top = Parent;
  }
  return Top;
}

ProgramPoint *MAIRLoopInfo::getPreheaderExit(const Loop &L) const {
  if (!Graph)
    return nullptr;
  BasicBlock *Preheader = L.getLoopPreheader();
  return Preheader ? &Graph->getBBExitRepresentative(*Preheader) : nullptr;
}

std::optional<MAIRLoopExit>
MAIRLoopInfo::describeUniqueExit(const Loop &L) const {
  BasicBlock *Exit = L.getUniqueExitBlock();
  BasicBlock *Exiting = L.getExitingBlock();
  if (!Exit || !Exiting)
    return std::nullopt;
  return MAIRLoopExit{Exiting, Exit, Exit->getUniquePredecessor() == Exiting};
}

ProgramPoint *MAIRLoopInfo::getUniqueExitEntry(const Loop &L) const {
  if (!Graph)
    return nullptr;
  BasicBlock *Exit = L.getUniqueExitBlock();
  return Exit ? &Graph->getBBEntryRepresentative(*Exit) : nullptr;
}

ScalarEvolution &MAIRLoopInfo::getSE() const {
  if (!FAM || !Func)
    report_fatal_error("MAIR LoopAnalysis requires a FunctionAnalysisManager");
  if (&FAM->getResult<llvm::LoopAnalysis>(*Func) != LI)
    report_fatal_error(
        "MAIR LoopAnalysis requires the MAIRGraph and ScalarEvolution to "
        "share LoopInfo");
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
