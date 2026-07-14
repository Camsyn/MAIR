#include "MAIR/Analysis/LoopAnalysis.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

AnalysisKey LoopAnalysis::Key;

MAIRLoopInfo::MAIRLoopInfo(MAIRGraph &G, FunctionAnalysisManager &Manager)
    : Graph(&G), FAM(&Manager), Func(const_cast<Function *>(G.getFunction())) {
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
  BasicBlock *Exit = L.getUniqueExitBlock();
  BasicBlock *Exiting = L.getExitingBlock();
  return Exit && Exiting && Exit->getUniquePredecessor() == Exiting;
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
  if (!FAM)
    report_fatal_error(
        "MAIR LoopAnalysis requires a FunctionAnalysisManager bridge");
  return MAIRLoopInfo(G, *FAM);
}

} // namespace mair
