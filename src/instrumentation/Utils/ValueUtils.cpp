#include "ValueUtils.h"
#include "MetaDataUtils.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace __xsan;
using namespace llvm;

namespace __xsan {

bool shouldSkip(const Instruction &I) {
  return isNoSanitize(I) || DelegateToXSan::is(I);
}

const llvm::Value *extractAddrFromLoadStoreInst(const llvm::Instruction &I) {
  if (auto *LI = dyn_cast<LoadInst>(&I)) {
    return LI->getPointerOperand();
  }
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    return SI->getPointerOperand();
  }
  return nullptr;
}

const Value *getUnderlyingObjectAggressive(const Value *V) {
  const unsigned MaxVisited = 8;

  SmallPtrSet<const Value *, 8> Visited;
  SmallVector<const Value *, 8> Worklist;
  Worklist.push_back(V);
  const Value *Object = nullptr;
  // Used as fallback if we can't find a common underlying object through
  // recursion.
  bool First = true;
  const Value *FirstObject = getUnderlyingObject(V);
  do {
    const Value *P = Worklist.pop_back_val();
    P = First ? FirstObject : getUnderlyingObject(P);
    First = false;

    if (!Visited.insert(P).second)
      continue;

    if (Visited.size() == MaxVisited)
      return FirstObject;

    if (auto *SI = dyn_cast<SelectInst>(P)) {
      Worklist.push_back(SI->getTrueValue());
      Worklist.push_back(SI->getFalseValue());
      continue;
    }

    if (auto *PN = dyn_cast<PHINode>(P)) {
      append_range(Worklist, PN->incoming_values());
      continue;
    }

    if (!Object)
      Object = P;
    else if (Object != P)
      return FirstObject;
  } while (!Worklist.empty());

  return Object ? Object : FirstObject;
}

static bool isVtableAccess(const Instruction &I) {
  if (MDNode *Tag = I.getMetadata(LLVMContext::MD_tbaa))
    return Tag->isTBAAVtableAccess();
  return false;
}

static bool isInReadOnlySection(const GlobalVariable &GV) {
  StringRef Section = GV.getSection();
  return Section.equals(".rodata") || Section.equals(".text") /* 其他只读段 */;
}

/// TODO: ensure this point: any direct/indirect reference to vtable is
/// unwritable.
// In most of the implementations of vtable, any direct/indirect reference of
// vtable is unwritable. However, the standard indeed permits a vtable pointer
// pointing to a writable region. Hence, this optimization is aggressive and
// depends on the implementation of vtable.
//
// The content of vtable is unwritable, including
//  - (direct) vptr->func_ptr
//  - (direct) vptr->type_info
//  - (indirect) vptr->type_info->name
//  - (indirect) vptr->type_info->name[0]
static bool belongToVtableAggresive(const Value *V) {
  static constexpr uint8_t MaxVtableDepth = 3;
  for (uint8_t Depth = 0; V && Depth < MaxVtableDepth; ++Depth) {
    V = __xsan::getUnderlyingObjectAggressive(V);
    if (const LoadInst *L = dyn_cast<LoadInst>(V)) {
      if (isVtableAccess(*L)) {
        return true;
      }
      V = L->getPointerOperand();
    } else {
      return false;
    }
  }
  return false;
}

bool addrPointsToConstantDataAggressive(const Value *Addr) {
  /// TODO: use more aggressive one, like `getUnderlyingObjectAggressive` in
  ///       LLVM 20
  Addr = __xsan::getUnderlyingObjectAggressive(Addr);
  // Check globals
  if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->isConstant() || isInReadOnlySection(*GV)) {
      // Reads from constant globals can not race with any writes.
      return true;
    }
  }
  // Check loads
  else if (const LoadInst *L = dyn_cast<LoadInst>(Addr)) {
    if (belongToVtableAggresive(L)) {
      // Reads from a vtable pointer can not race with any writes.
      return true;
    }
  }

  return false;
}

static bool isByValArgument(const Value *V) {
  if (const Argument *A = dyn_cast<Argument>(V))
    return A->hasByValAttr();
  return false;
}

/// `alloc` is handled in other place, this function is for other cases.
/// 1. `byval` pointer argument
/// 2. `noalias` call return value
/// 3. `alloc` stack object
/// Note that `noalias` argument should have been a function local, but because
/// it belongs to capture by return in the context of TSan, it is not included.
bool isUncapturedFuncLocal(const Value &Addr) {
  const Value *Underlying = getUnderlyingObjectAggressive(&Addr);
  bool IsFuncLocal = isa<AllocaInst>(Underlying) || isNoAliasCall(Underlying) ||
                     isByValArgument(Underlying);
  if (!IsFuncLocal)
    return false;
  return !PointerMayBeCaptured(Underlying, true, true);
}

// If the instruction is immediately followed by a terminator and the block has
// only a single successor, return that successor. Otherwise, split the BB after
// this instruction and return the new split BB address. Using llvm::SplitBlock
// to hot-update some analysis results.
BlockAddress *getBlockAddressOfInstruction(Instruction &I, DominatorTree *DT,
                                           LoopInfo *LI,
                                           MemorySSAUpdater *MSSAU) {
  BasicBlock *OriginalBb = I.getParent();

  // If the next instruction is at the end of the BB and there is only single
  // successor, just return the successor BB
  Instruction *NextI = I.getNextNode();
  if (NextI && NextI->isTerminator()) {
    auto *SingleSuccessor = NextI->getParent()->getSingleSuccessor();
    if (SingleSuccessor) {
      return BlockAddress::get(SingleSuccessor->getParent(), SingleSuccessor);
    }
  }

  // or split the block.
  // NewBb is the BB after the instruction I.
  BasicBlock *NewBb =
      llvm::SplitBlock(OriginalBb, NextI, DT, LI, MSSAU, "mop.address", false);
  return BlockAddress::get(NewBb->getParent(), NewBb);
}

// ref: llvm/lib/Analysis/ValueTracking.cpp
Instruction *findAllocaForValue(Value *V, bool OffsetZero) {
  Instruction *Result = nullptr;
  SmallPtrSet<Value *, 4> Visited;
  SmallVector<Value *, 4> Worklist;

  auto AddWork = [&](Value *V) {
    if (Visited.insert(V).second)
      Worklist.push_back(V);
  };

  AddWork(V);
  do {
    V = Worklist.pop_back_val();
    assert(Visited.count(V));

    if (Instruction * AI;
        (AI = dyn_cast<AllocaInst>(V)) ||
        ((AI = dyn_cast<Instruction>(V)) && ReplacedAlloca::is(*AI))) {
      if (Result && Result != AI)
        return nullptr;
      Result = AI;
    } else if (CastInst *CI = dyn_cast<CastInst>(V)) {
      AddWork(CI->getOperand(0));
    } else if (PHINode *PN = dyn_cast<PHINode>(V)) {
      for (Value *IncValue : PN->incoming_values())
        AddWork(IncValue);
    } else if (auto *SI = dyn_cast<SelectInst>(V)) {
      AddWork(SI->getTrueValue());
      AddWork(SI->getFalseValue());
    } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V)) {
      if (OffsetZero && !GEP->hasAllZeroIndices())
        return nullptr;
      AddWork(GEP->getPointerOperand());
    } else if (CallBase *CB = dyn_cast<CallBase>(V)) {
      Value *Returned = CB->getReturnedArgOperand();
      if (Returned)
        AddWork(Returned);
      else
        return nullptr;
    } else {
      return nullptr;
    }
  } while (!Worklist.empty());

  return Result;
}

} // namespace __xsan
