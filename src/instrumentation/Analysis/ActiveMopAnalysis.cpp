//===- ActiveMopAnalysis.cpp - Active Memory Operations Analysis -*-C++-*--===//
//
// This file implements the ReachableMOPs class, which performs a static
// dataflow analysis to determine reachable Memory Operations (MOPs) within an
// LLVM Function. The analysis uses a worklist algorithm and leverages LLVM's
// data structures for efficiency.
//
//===----------------------------------------------------------------------===//

#include "ActiveMopAnalysis.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h" // For llvm::reverse
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace llvm;

namespace __xsan {

/// @brief Checks if an instruction clobbers memory.
/// - no-side-effected-calls between MOP1 and MOP2
static bool isClobberingCall(const Instruction *Inst,
                             bool IgnoreNoSanitized = true) {
  // Check call/invoke instruction
  if (!isa<CallBase>(Inst)) {
    return false;
  }

  if (isa<DbgInfoIntrinsic>(Inst)) {
    return false;
  }
  // Check call decorated with non-sanitize metadata
  /// TODO: use other metadata? nosanitize metadata might be used by users.
  if (IgnoreNoSanitized && Inst->hasMetadata(LLVMContext::MD_nosanitize))
    return false;

  if (!Inst->mayWriteToMemory())
    return false;

  return true;
}


static AtomicOrdering getOrder(const Instruction *I) {
  // TODO: Ask TTI whether synchronization scope is between threads.
  auto SSID = getAtomicSyncScopeID(I);
  if (!SSID)
    return AtomicOrdering::NotAtomic;
  // SyncScope = SingleThread --> NotAtomic for TSan
  if ((isa<LoadInst>(I) || isa<StoreInst>(I)) &&
      (SSID.value() == SyncScope::SingleThread))
    return AtomicOrdering::NotAtomic;

  if (const auto *LI = dyn_cast<LoadInst>(I)) {
    return LI->getOrdering();
  }
  if (const auto *SI = dyn_cast<StoreInst>(I)) {
    return SI->getOrdering();
  }
  if (const auto *RMWI = dyn_cast<AtomicRMWInst>(I)) {
    return RMWI->getOrdering();
  }
  if (const auto *CXI = dyn_cast<AtomicCmpXchgInst>(I)) {
    // CAS : read & write if success, only read if failure.
    AtomicOrdering SuccessOrder = CXI->getSuccessOrdering();
    AtomicOrdering FailureOrder = CXI->getFailureOrdering();
    if (SuccessOrder == FailureOrder)
      return SuccessOrder;
    // Memory Order Set is partially ordered as follows:
    //   not_atomic-->unordered-->relaxed-->release-->acq_rel-->seq_cst
    //                        |-->acquire-->|
    // Return the least upper bound of the two orders
    if (SuccessOrder == AtomicOrdering::Release ||
        SuccessOrder == AtomicOrdering::Acquire) {
      return AtomicOrdering::AcquireRelease;
    }
    using u8 = unsigned char;
    return static_cast<AtomicOrdering>(
        std::max(static_cast<u8>(SuccessOrder), static_cast<u8>(FailureOrder)));
  }
  if (const auto *FENCE = dyn_cast<FenceInst>(I)) {
    /// TODO: TSan does not support handling of fences for now. Therefore, we
    /// just return NotAtomic.
    return AtomicOrdering::NotAtomic;
  }

  return AtomicOrdering::NotAtomic;
}

/// TSan is sensitive to synchronization intruction as follows:
/// - atomic load/store
/// - fence
/// - atomic cmpxchg
/// - atomicrmw
/// except their memory order is relaxed.
/// Notably, TSan disables ClInstrumentReadBeforeWrite by default, which
/// does not consider atomic operations/fences, leading to false negatives.
static void getAtomicInsnOrder(const Instruction *Inst, bool &IsAcq,
                               bool &IsRel) {
  AtomicOrdering Order = getOrder(Inst);
  switch (Order) {
  case AtomicOrdering::NotAtomic:
  case AtomicOrdering::Unordered:
  case AtomicOrdering::Monotonic:
    return;
  case AtomicOrdering::Acquire:
    IsAcq = true;
    return;
  case AtomicOrdering::Release:
    IsRel = true;
    return;
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    IsAcq = true;
    IsRel = true;
    return;
  }
}

/// @brief Checks if one instruction is reachable from another within the same
/// basic block.
static bool isActiveInTheSameBlock(const Instruction *From,
                                   const Instruction *To) {
  const Instruction *Inst = From;
  do {
    if (Inst == To) {
      return true;
    }
    if (isClobberingCall(Inst)) {
      return false;
    }
  } while ((Inst = compat::getNextNonDebugInstruction(Inst)));

  llvm_unreachable("From and To should be in the same basic block and FromI "
                   "should be before ToI");
}

static bool isActiveInTheSameBlock(const Instruction *From,
                                   const Instruction *To, const bool Acq) {
  const Instruction *Inst = From;
  bool HasAcq = false, HasRel = false;
  do {
    if (Inst == To) {
      return true;
    }
    if (isClobberingCall(Inst)) {
      return false;
    }
    getAtomicInsnOrder(Inst, HasAcq, HasRel);
    if (Acq && HasAcq || !Acq && HasRel) {
      return false;
    }
  } while ((Inst = compat::getNextNonDebugInstruction(Inst)));

  llvm_unreachable("From and To should be in the same basic block and FromI "
                   "should be before ToI");
}

ActiveMopAnalysis::Lattice::Lattice(unsigned NumMops, bool UsedForTsan)
    : Reachable(NumMops, false), NotActive(NumMops, false),
      AnyAcq(UsedForTsan ? NumMops : 0, false),
      AnyRel(UsedForTsan ? NumMops : 0, false), UsedForTsan(UsedForTsan) {}

void ActiveMopAnalysis::Lattice::kill() {
  /// ⊥ → ⊥, else -> ⊤
  /// Can be implemented as NotActive = Reachable.
  NotActive = Reachable;
}

void ActiveMopAnalysis::Lattice::killAcq() {
  if (LLVM_UNLIKELY(!UsedForTsan)) {
    return;
  }
  /// ⊥ → ⊥, else -> A
  /// Can be implemented as AnyAcq = Reachable.
  AnyAcq = Reachable;
}

void ActiveMopAnalysis::Lattice::killRel() {
  if (LLVM_UNLIKELY(!UsedForTsan)) {
    return;
  }
  /// ⊥ → ⊥, else -> R
  /// Can be implemented as AnyRel = Reachable.
  AnyRel = Reachable;
}

void ActiveMopAnalysis::Lattice::join(const Lattice &Other) {
  Reachable |= Other.Reachable;
  NotActive |= Other.NotActive;
  if (UsedForTsan) {
    AnyAcq |= Other.AnyAcq;
    AnyRel |= Other.AnyRel;
  }
}

bool ActiveMopAnalysis::Lattice::operator==(const Lattice &Other) const {
  return Reachable == Other.Reachable && NotActive == Other.NotActive &&
         (!UsedForTsan || (AnyAcq == Other.AnyAcq && AnyRel == Other.AnyRel));
}

SmallVector<const Instruction *, 64>
ActiveMopAnalysis::extractMOPs(const Function &F) const {
  SmallVector<const Instruction *, 64> MOPs;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (isInterestingMop(I)) {
        MOPs.push_back(&I);
      }
    }
  }
  return MOPs;
}

void ActiveMopAnalysis::initializeMopMaps() {
  MopMap.clear();

  unsigned Id = 0;

  for (auto *I : MopList) {
    MopMap[I] = Id;
    Id++;
  }
}

ActiveMopAnalysis::MopID
ActiveMopAnalysis::getMopId(const Instruction *I) const {
  auto it = MopMap.find(I);
  assert(it != MopMap.end() && "Instruction does not exist in MopMap");
  return it->second;
}

void ActiveMopAnalysis::initializeBlockInfo(const Function &F) {
  unsigned NumMoPs = MopList.size();

  for (const BasicBlock &BB : F) {
    BlockInfo Val = getInitializedBlockInfo(BB);
    BlockInfoMap.try_emplace(&BB, std::move(Val));
  }
}

ActiveMopAnalysis::BlockInfo
ActiveMopAnalysis::getInitializedBlockInfo(const BasicBlock &BB) const {
  unsigned NumMops = MopList.size();
  BitVectorSet GenSet(NumMops, false);
  BitVectorSet GenAcqSet(UsedForTsan ? NumMops : 0, false);
  BitVectorSet GenRelSet(UsedForTsan ? NumMops : 0, false);

  BitVectorSet UseGen(NumMops, false);
  BitVectorSet NotUseGen(NumMops, false);

  bool ContainsCall = false;
  bool ContainsAcq = false, ContainsRel = false;

  // Traverse instructions in reverse to find MOPs that are not followed by
  // calls.
  for (const Instruction &I : reverse(BB)) {
    if (!ContainsCall && isClobberingCall(&I)) {
      ContainsCall = true;
    }
    if (UsedForTsan) {
      getAtomicInsnOrder(&I, ContainsAcq, ContainsRel);
    }
    if (!isInterestingMop(I)) {
      continue;
    }
    auto MapIt = MopMap.find(&I);
    if (MapIt == MopMap.end())
      continue;
    MopID MopId = MapIt->second;
    UseGen.set(MopId);
    NotUseGen.set(MopId);
    if (ContainsCall) {
      GenSet.set(MopId);
    }
    if (LLVM_UNLIKELY(ContainsAcq)) {
      GenAcqSet.set(MopId);
    }
    if (LLVM_UNLIKELY(ContainsRel)) {
      GenRelSet.set(MopId);
    }
  }

  NotUseGen.flip();

  const BlockInvariant Invariant = {std::move(UseGen),    std::move(NotUseGen),
                                    std::move(GenSet),    std::move(GenAcqSet),
                                    std::move(GenRelSet), ContainsCall,
                                    ContainsAcq,          ContainsRel};

  return {Lattice(NumMops, UsedForTsan), Lattice(NumMops, UsedForTsan),
          Invariant};
}

const ActiveMopAnalysis::BlockInfo &
ActiveMopAnalysis::getBlockInfo(const BasicBlock *BB) const {
  auto It = BlockInfoMap.find(BB);
  assert(It != BlockInfoMap.end() &&
         "BasicBlock does not exist in BlockInfoMap");
  return It->getSecond();
}

ActiveMopAnalysis::BlockInfo &
ActiveMopAnalysis::getBlockInfo(const BasicBlock *BB) {
  auto It = BlockInfoMap.find(BB);
  assert(It != BlockInfoMap.end() &&
         "BasicBlock does not exist in BlockInfoMap");
  return It->getSecond();
}

bool ActiveMopAnalysis::isMopActiveAfter(MopID Mop, const BasicBlock *BB,
                                         const bool Acq) const {
  const BlockInfo &Info = getBlockInfo(BB);
  return UsedForTsan ? Info.OUT.isActive(Mop, Acq) : Info.OUT.isActive(Mop);
}

bool ActiveMopAnalysis::isMopActiveBefore(MopID Mop, const BasicBlock *BB,
                                          const bool Acq) const {
  const BlockInfo &Info = getBlockInfo(BB);
  return UsedForTsan ? Info.IN.isActive(Mop, Acq) : Info.IN.isActive(Mop);
}

void ActiveMopAnalysis::printInfo(const BlockInfo &Info,
                                  raw_ostream &OS) const {
  OS << "  IN: \n";
  for (MopID id = 0; id < MopList.size(); ++id) {
    if (Info.IN.isActive(id)) {
      OS << "\t" << *MopList[id] << "\n";
    }
  }
  OS << "\n";

  OS << "  OUT: \n";
  for (MopID id = 0; id < MopList.size(); ++id) {
    if (Info.OUT.isActive(id)) {
      OS << "\t" << *MopList[id] << "\n";
    }
  }
  OS << "------\n";
}

bool ActiveMopAnalysis::suitableToAnlyze() {
  if (!MopList.size()) {
    return false;
  }

  bool CrossBlocks = false;
  const BasicBlock *BB = MopList[0]->getParent();
  for (const auto *I : MopList) {
    if (I->getParent() != BB) {
      CrossBlocks = true;
      break;
    }
  }
  return CrossBlocks;
}

ActiveMopAnalysis::ActiveMopAnalysis(
    Function &F, const SmallVectorImpl<const Instruction *> &MOPs,
    bool UsedForTsan)
    : MopList(MOPs.begin(), MOPs.end()), F(F), Analyzed(false),
      UsedForTsan(UsedForTsan) {
  if (suitableToAnlyze())
    dataflowAnalyze();
}

ActiveMopAnalysis::ActiveMopAnalysis(Function &F, bool UsedForTsan)
    : ActiveMopAnalysis(F, extractMOPs(F), UsedForTsan) {}

bool ActiveMopAnalysis::updateInfo(const BasicBlock *BB) {
  BlockInfo &Info = getBlockInfo(BB);

  // Merge
  /// IN[B] = ∪_{P ∈ pre(B)} OUT[P]
  for (const BasicBlock *Pred : predecessors(BB)) {
    const BlockInfo &PredInfo = getBlockInfo(Pred);
    Info.IN.join(PredInfo.OUT);
  }

  // Transfer
  /// OUT = UseGen? Gen : (HasCall? Kill(IN) : IN)
  Lattice NewOUT = transfer(Info);

  if (NewOUT == Info.OUT) {
    return false;
  }

  Info.OUT = std::move(NewOUT);
  return true;
}

ActiveMopAnalysis::Lattice
ActiveMopAnalysis::transfer(const BlockInfo &Info) const {
  /// Implace version of tranfer function.
  Lattice NewOUT = Info.IN;

  const BlockInvariant &Invariant = Info.Invariant;
  // OUT = (HasCall? Kill(IN) : IN)
  // => OUT = IN
  //    if (UseKill) OUT.kill()
  if (Invariant.ContainsCall) {
    NewOUT.kill();
  }
  if (UsedForTsan) {
    if (LLVM_UNLIKELY(Invariant.ContainsAcq)) {
      NewOUT.killAcq();
    }
    if (LLVM_UNLIKELY(Invariant.ContainsRel)) {
      NewOUT.killRel();
    }
  }

  // OUT = UseGen? Gen : (HasCall? Kill(IN) : IN)
  // ==>
  //       Gen = {~, ⊤} --> {10, 11}
  //    UseGen = 1 -->  OUT.reach = 1
  NewOUT.Reachable |= Invariant.UseGen;
  // OUT = UseGen? Gen : (HasCall? Kill(IN) : IN)
  // ==> OUT = UseGen? Gen : OUT
  // ==> OUT = (UseGen & Gen) | (~UseGen & OUT)
  // ==> OUT &= ~UseGen
  //     OUT |= (UseGen & Gen) # Gen = UseGen & Gen naturally
  NewOUT.NotActive &= Invariant.NotUseGen;
  NewOUT.NotActive |= Invariant.Gen;

  // Similarly
  if (UsedForTsan) {
    NewOUT.AnyAcq &= Invariant.NotUseGen;
    NewOUT.AnyAcq |= Invariant.GenAcq;
    NewOUT.AnyRel &= Invariant.NotUseGen;
    NewOUT.AnyRel |= Invariant.GenRel;
  }

  return NewOUT;
}

/// TODO: SCC and DAG
void ActiveMopAnalysis::dataflowAnalyze() {
  if (F.isDeclaration() || F.empty())
    return;

  initializeMopMaps();
  initializeBlockInfo(F);

  // Initialize the worklist using SmallSetVector to avoid duplicates.
  SmallSetVector<const BasicBlock *, 32> WorkList;

  // Use reverse post-order traversal first to speed up the iteration.
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  for (const auto *BB : RPOT) {
    // Erase BB from worklist as we will update its info in the next line.
    WorkList.remove(BB);

    // Update the block info for BB.
    updateInfo(BB);

    // Propagate to successors.
    // As we erase BB in the beginning, the enrties of
    // loops will retain after the ReverserPostOrderTraversal ends.
    for (const BasicBlock *Succ : successors(BB))
      WorkList.insert(Succ);
  }

  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.pop_back_val();

    if (!updateInfo(BB)) {
      continue;
    }

    // Propagate changes to successors.
    for (const BasicBlock *Succ : successors(BB)) {
      WorkList.insert(Succ);
    }
  }

  Analyzed = true;
}

bool ActiveMopAnalysis::isOneMopActiveToAnother(const Instruction *From,
                                                const Instruction *To,
                                                const bool IsToDead) const {
  const BasicBlock *FromBB = From->getParent();
  const BasicBlock *ToBB = To->getParent();

  /*
    If KillingI dominates DeadI, i.e., From is killing and To is dead,
    KillingI is not active to DeadI if exists any RELEASE barrier between them.

    If KillingI post-dominate DeadI, i.e., To is killing and From is dead,
    KillingI is not active to DeadI if exists any ACQUIRE barrier between them.
  */
  const bool ConsiderAcqOrRel = !IsToDead;
  const bool UsedForTsan = this->UsedForTsan;

  const auto isOneActiveToAnotherInTheSameBlock =
      [UsedForTsan, ConsiderAcqOrRel](const Instruction *KillingI,
                                      const Instruction *DeadI) {
        return UsedForTsan
                   ? isActiveInTheSameBlock(KillingI, DeadI, ConsiderAcqOrRel)
                   : isActiveInTheSameBlock(KillingI, DeadI);
      };

  if (!Analyzed) {
    assert(FromBB == ToBB && "From and To should be in the same basic block");
    return isOneActiveToAnotherInTheSameBlock(From, To);
  }

  MopID FromId = getMopId(From);

  // If a MOP is reachable in the OUT[ToBB], then it must be reachable for To.
  if (isMopActiveAfter(FromId, ToBB, ConsiderAcqOrRel))
    return true;

  // If From and To belong to the same basic block, then we can use the
  // internal reachability test.
  if (FromBB == ToBB) {
    return isOneActiveToAnotherInTheSameBlock(From, To);
  }

  // If the external From is not reachable in the IN[ToBB], then it must not be
  // reachable for To.
  if (!isMopActiveBefore(FromId, ToBB, ConsiderAcqOrRel)) {
    assert(ToBB != FromBB && "ToBB should not be the same as FromBB");
    return false;
  }

  // Eventually, we need to check if the entry of the ToBB can reach To.
  const Instruction *Entry = __xsan::compat::getFirstNonPHIOrDbgOrLifetime(*ToBB);
  if (!Entry)
    return false;
  return isOneActiveToAnotherInTheSameBlock(Entry, To);
}

const SmallVector<const Instruction *, 64> &
ActiveMopAnalysis::getMOPList() const {
  return MopList;
}

void ActiveMopAnalysis::printAnalysis(raw_ostream &OS) {
  if (F.empty() || F.isDeclaration()) {
    OS << "Function is empty or is a declaration\n";
    return;
  }

  if (!Analyzed) {
    dataflowAnalyze();
  }

  // Print MopList
  OS << "------------------------\n";
  OS << "MopList:\n";
  for (const Instruction *I : MopList) {
    OS << "\t" << *I << "\n";
  }
  OS << "------------------------\n";

  for (const auto &[BB, Info] : BlockInfoMap) {
    OS << "BasicBlock: ";
    BB->printAsOperand(OS);
    OS << "\n";

    printInfo(Info, OS);
    OS << "\n";
  }
}

} // namespace __xsan
