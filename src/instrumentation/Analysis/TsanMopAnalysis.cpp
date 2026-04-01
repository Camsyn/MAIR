#include "TsanMopAnalysis.h"
#include "../Utils/ValueUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h" // For llvm::reverse
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
static bool isStackObject(const Value *Addr) {
  return (Addr != nullptr) ? isa<AllocaInst>(getUnderlyingObject(Addr)) : false;
}

using BlockSet = df_iterator_default_set<BasicBlock *, 16>;

static SmallVector<const AllocaInst *, 16> getStackObjects(const Function &F) {
  const BasicBlock &EntryBB = F.getEntryBlock();
  SmallVector<const AllocaInst *, 16> StackObjects;
  for (const Instruction &I : EntryBB) {
    if (auto *AI = dyn_cast<AllocaInst>(&I)) {
      StackObjects.push_back(AI);
    }
  }
  return StackObjects;
}

static BlockSet depthFirstTraversal(ArrayRef<BasicBlock *> Starts) {
  BlockSet Visited;
  for (BasicBlock *Start : Starts) {
    auto I = df_ext_begin(Start, Visited);
    auto E = df_ext_end(Start, Visited);
    for (auto *BB : depth_first_ext(Start, Visited)) {
      (void)BB; // Suppress unused variable warning.
    }
  }
  return Visited;
}

static BlockSet
inverseDepthFirstTraversal(ArrayRef<BasicBlock *> Starts,
                           function_ref<bool(BasicBlock *)> Prune) {
  BlockSet Visited;
  for (BasicBlock *Start : Starts) {
    auto I = idf_begin(Start);
    auto E = idf_end(Start);
    for (; I != E;) {
      BasicBlock *BB = *I;
      if (Prune(BB) || Visited.contains(BB)) {
        I.skipChildren();
        continue;
      }
      Visited.insert(BB);
      ++I;
    }
  }
  return Visited;
}

class StackRaceAnalyzer {
  // > Threshould : use 2 DFS; <= Threshould : just query by brute force
  constexpr static unsigned MopThreshold = 2;

public:
  StackRaceAnalyzer(const Function &F, const MemorySSA &MSSA,
                    const DominatorTree &DT, const LoopInfo &LI,
                    const AAResults &AA)
      : F(F), MSSA(MSSA), DT(DT), LI(LI), AA(AA),
        DL(F.getParent()->getDataLayout()) {}

  SmallPtrSet<const Instruction *, 8>
  allMopsMightRace(const AllocaInst &Alloc) {
    SmallVector<const Instruction *, 8> CandidateMops;
    SmallVector<const CallBase *, 8> MightSpawns, MightJoins;
    walkOverAllocaDefUseChains(Alloc, CandidateMops, MightSpawns, MightJoins);
    if (CandidateMops.empty() || MightSpawns.empty() || MightJoins.empty()) {
      return {};
    }

    SmallPtrSet<const Instruction *, 8> MopsMayRace;
    /// All MOPs between any MightSpawn to any MightJoin are racy.
    if (CandidateMops.size() <= MopThreshold) {
      collectSeveralMopsMayRace(MopsMayRace, CandidateMops, MightSpawns,
                                MightJoins);
    } else {
      collectManyMopsMayRace(MopsMayRace, CandidateMops, MightSpawns,
                             MightJoins);
    }
    errs() << "Addr: " << Alloc << "\n";
    for (const Instruction *Mop : MopsMayRace) {
      errs() << "\tMop: " << *Mop << "\n";
    }
    errs() << "\n";

    return MopsMayRace;
  }

private:
  // Walk over the def-use chains of the alloca instruction to fill in the
  // following containers:
  //   - `CandidateMops`: all load/store instructions accessing the alloca
  //   - `MightSpawns`  : all call instructions that might create thread and use
  //                      the alloca instruction as a parameter
  //   - `MightJoins`   : all call instructions that might join thread and
  //                      receive the thread id as a parameter
  // Note that there are THREE dataflow from alloca addr to MightSpawn as
  // follows:
  //   1. alloca -> argument -> func
  //   2. alloca -> structure -> argument -> func
  //   3. alloca -> global -> func
  void
  walkOverAllocaDefUseChains(const AllocaInst &Alloc,
                             SmallVector<const Instruction *, 8> &CandidateMops,
                             SmallVector<const CallBase *, 8> &MightSpawns,
                             SmallVector<const CallBase *, 8> &MightJoins) {
    SmallVector<const Use *, 20> Worklist;
    SmallPtrSet<const Use *, 16> Visited;
    auto AddUses = [&](const Value &V) {
      for (const Use &U : V.uses()) {
        // If there are lots of uses, conservatively say that the value
        // is captured to avoid taking too much compile time.
        if (!Visited.insert(&U).second)
          continue;
        Worklist.push_back(&U);
      }
    };
    AddUses(Alloc);
    /// Those store instructions that store the alloca addr to other places
    SmallVector<const StoreInst *, 8> Addr2Stores;

    /// TODO: could we reuse the API llvm::PointerMayBeCaptured ?
    // Traverse the def-use chains of the alloca instruction to collect all
    // related instructions and calls.
    while (!Worklist.empty()) {
      const Use *U = Worklist.pop_back_val();
      const Instruction *Inst = dyn_cast<Instruction>(U->getUser());
      if (!Inst)
        continue;
      if (__xsan::isNoSanitize(*Inst))
        continue;
      switch (Inst->getOpcode()) {
        case Instruction::Call:
        case Instruction::CallBr:
        case Instruction::Invoke: {
          const CallBase *CB = cast<CallBase>(Inst);
          if (maySpawnThread(*CB, *U)) {
            MightSpawns.push_back(CB);
          }
          if (isIntrinsicReturningPointerAliasingArgumentWithoutCapturing(CB, true)) {
            AddUses(*Inst);
          }

          if (const auto *MI = dyn_cast<MemIntrinsic>(Inst)) {

          }

          if (CB->isCallee(U)) {
            break;
          }
        }

        case Instruction::Store:
          if (U->getOperandNo() == 0) break;
        case Instruction::Load:
          CandidateMops.push_back(Inst);
          break;
        case Instruction::AtomicRMW:
        case Instruction::AtomicCmpXchg:
        case Instruction::BitCast:
        case Instruction::GetElementPtr:
        case Instruction::PHI:
        case Instruction::Select:
        case Instruction::AddrSpaceCast:
          AddUses(*Inst);
        default:
           break;
      }
    }

    for (auto &I : instructions(F)) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        // Call instruction is also terminator.
        // We do not track arg->ret chain, and conservasively assume all MOP
        // related to the addr returned by function call might be racy.
        if (mayJoinThread(*CB, MightSpawns)) {
          MightJoins.push_back(CB);
        }
      }
    }
  }

  void
  collectSeveralMopsMayRace(SmallPtrSet<const Instruction *, 8> &MopsMayRace,
                            ArrayRef<const Instruction *> Mops,
                            ArrayRef<const CallBase *> MightSpawns,
                            ArrayRef<const CallBase *> MightJoins) {
    for (const Instruction *Mop : Mops) {
      if (!isPotentiallyReachable(MightSpawns, Mop)) {
        continue;
      }
      if (!isPotentiallyReachable(Mop, MightJoins)) {
        continue;
      }
      MopsMayRace.insert(Mop);
    }
  }

  void collectManyMopsMayRace(SmallPtrSet<const Instruction *, 8> &MopsMayRace,
                              ArrayRef<const Instruction *> Mops,
                              ArrayRef<const CallBase *> MightSpawns,
                              ArrayRef<const CallBase *> MightJoins) {
    auto Inst2BB = [](const Instruction *Inst) {
      return const_cast<BasicBlock *>(Inst->getParent());
    };

    SmallVector<BasicBlock *, 8> BegBBs(map_range(MightSpawns, Inst2BB));
    SmallVector<BasicBlock *, 8> EndBBs(map_range(MightJoins, Inst2BB));

    BlockSet ForwardBBs = depthFirstTraversal(BegBBs);
    BlockSet BlocksBetween =
        inverseDepthFirstTraversal(EndBBs, [&](BasicBlock *BB) {
          // If ForwardBBs does not contain BB, it means BB is not reachable
          // from any of the `MightSpawns`. So we can prune this branch from the
          // search.
          return !ForwardBBs.contains(BB);
        });

    /*
    An upper bound is a BB that contains a might-spawn call instruction, and any
    of its predecessors cannot be reached from any upper bound. A lower bound is
    a BB that contains a might-join call instruction, and any of its successors
    cannot be reached from any lower bound.

    e.g.,
        ```c
        thread_create(p);   // upper bound
        thread_join(p);     // lower bound
        ```
        ```c
        while(...) {
            thread_create(p);   // not an upper bound
            thread_join(p);     // not a lower bound
        }
        ```
    */
    auto IsBlockBetween = [&](const BasicBlock *BB) -> bool {
      return BlocksBetween.contains(BB);
    };
    auto DeriveBounds = [&](bool IsUpperBounds,
                            ArrayRef<const CallBase *> OrigBounds) {
      DenseMap<const BasicBlock *, const Instruction *> Bounds;
      for (const CallBase *CB : OrigBounds) {
        const BasicBlock *BB = CB->getParent();
        if (IsUpperBounds ? any_of(predecessors(BB), IsBlockBetween)
                          : any_of(successors(BB), IsBlockBetween)) {
          continue;
        }

        auto II = Bounds.insert({BB, CB});
        if (!II.second) {
          const auto *LastCB = II.first->second;
          bool IsAfter = LastCB->comesBefore(CB);
          // If deriving upper bounds, the fronter one priorities.
          // If deriving lower bounds, the later one priorities.
          // --> IsAfter ^ UpperBounds --> CB
          II.first->second = (IsAfter ^ IsUpperBounds) ? CB : LastCB;
        }
      }
      return Bounds;
    };

    DenseMap<const BasicBlock *, const Instruction *> UpperBounds =
        DeriveBounds(true, MightSpawns);
    DenseMap<const BasicBlock *, const Instruction *> LowerBounds =
        DeriveBounds(false, MightJoins);

    /*
    If MOP is in the bound BB, but must not race with the bound instruction,
    return true.
    e.g., MOP comes before the upper bound, or comes after the lower bound.
        ```c
        *p = 0;             // no race here
        thread_create(p);   // upper bound
        ...                 // multiple blocks...
        thread_join(p);     // lower bound
        *p = 1;             // no race here
        ```
    */
    auto MustNoRaceInBounds = [&](const Instruction *Mop, bool IsUpperBounds) {
      const BasicBlock *MopBB = Mop->getParent();
      auto It =
          IsUpperBounds ? UpperBounds.find(MopBB) : LowerBounds.find(MopBB);
      if (It == (IsUpperBounds ? UpperBounds.end() : LowerBounds.end())) {
        return false;
      }
      const Instruction *Bound = It->second;
      return IsUpperBounds ? Mop->comesBefore(Bound) : Bound->comesBefore(Mop);
    };

    // \forall s \in  MightSpawns
    //    \forall j \in  MightJoins
    //       \forall path \in  InstPaths(s,j)
    //          \forall i \in path,
    //               MOP = i
    // \implies
    //       MOP might race
    for (const Instruction *Mop : Mops) {
      const BasicBlock *MopBB = Mop->getParent();
      if (!BlocksBetween.contains(MopBB)) {
        continue;
      }
      if (MustNoRaceInBounds(Mop, true) || MustNoRaceInBounds(Mop, false)) {
        continue;
      }
      MopsMayRace.insert(Mop);
    }
  }

  bool isPotentiallyReachable(ArrayRef<const CallBase *> Froms,
                              const Instruction *To) {
    const BasicBlock *ToBB = To->getParent();
    SmallVector<BasicBlock *, 8> FromBBs;
    for (const Instruction *From : Froms) {
      BasicBlock *FromBB = const_cast<BasicBlock *>(From->getParent());
      if (FromBB == ToBB) {
        if (LI.getLoopFor(FromBB) != nullptr || From->comesBefore(To)) {
          return true;
        }
        continue;
      }
      FromBBs.push_back(FromBB);
    }

    if (FromBBs.empty()) {
      return false;
    }

    assert(find(FromBBs, ToBB) == FromBBs.end() &&
           "ToBB should not be in FromBBs");

    return isPotentiallyReachableFromMany(
        FromBBs, const_cast<BasicBlock *>(ToBB), nullptr, &DT, &LI);
  }

  bool isPotentiallyReachable(const Instruction *From,
                              ArrayRef<const CallBase *> Tos) {
    const BasicBlock *FromBB = From->getParent();
    SmallPtrSet<const BasicBlock *, 8> ToBBs;
    for (const Instruction *To : Tos) {
      const BasicBlock *ToBB = To->getParent();
      if (FromBB == ToBB) {
        if (LI.getLoopFor(FromBB) != nullptr || From->comesBefore(To)) {
          return true;
        }
        continue;
      }
      ToBBs.insert(ToBB);
    }

    if (ToBBs.empty()) {
      return false;
    }

    assert(!ToBBs.contains(FromBB) && "FromBB should not be in ToBBs");

    return any_of(depth_first(FromBB),
                  [&](const BasicBlock *BB) { return ToBBs.contains(BB); });
  }

  /// Feature of those suspicious thread creation call:
  ///   - is a call instruction
  ///   - receive the alloca instruction as a parameter
  ///   - return a thread id (i64) or receive a thread id pointer (i64*) as a
  ///     parameter
  bool maySpawnThread(const CallBase &CB, const Use &U, bool FromGlobal = false) {
    if (__xsan::shouldSkip(CB))
      return false;

    // There is no intrinsic that creates a thread.
    if (isa<IntrinsicInst>(CB))
      return false;

    if (FromGlobal && !CB.mayReadFromMemory()) {
      return false;
    }

    // Check whether the alloc might be captured by the CB, if not, the call cannot
    // spawn a thread accessing the alloc.
    if (!FromGlobal &&
        (!CB.isDataOperand(&U) || CB.doesNotCapture(CB.getDataOperandNo(&U)))) {
      return false;
    }

    // If a CB might write to global memory, conservatively estimate it
    // is a threads creation function.
    /// TODO: fit LLVM16's more precise memory model
    if (CB.mayWriteToMemory()) {
      return true;
    }


    // Check the return type
    if (Type *RetTy = CB.getType()) {
      if (containsI64(RetTy))
        return true;
    }

    // Check the argument type
    for (const Value *Arg : CB.args()) {
      if (containsSuspiciousPointer(Arg->getType()))
        return true;
    }

    return false;
  }

  /// Feature of those suspicious thread creation call:
  ///   - receive i64 / i64* as parameters
  ///   - the argument comes from the `MightSpawnCalls`
  bool mayJoinThread(const CallBase &CB,
                     const SmallVectorImpl<const CallBase *> &MightSpawnCalls) {
    if (__xsan::shouldSkip(CB))
      return false;
    // There is no intrinsic that joins a thread.
    if (isa<IntrinsicInst>(CB))
      return false;

    // If a CB might read from global memory, conservatively estimate it
    // is a threads join function.
    /// TODO: fit LLVM 16's more precise memory model
    if (CB.mayReadFromMemory()) {
      return true;
    }

    /// TODO: complete the logic of checking the argument type
    /// TODO: use MSSA
    return true;
  }

  bool containsI64(Type *Ty) {
    if (!Ty)
      return false;
    if (Ty->isIntegerTy(64))
      return true;
    if (auto *STy = dyn_cast<StructType>(Ty)) {
      for (Type *ElemTy : STy->elements()) {
        if (containsI64(ElemTy))
          return true;
      }
    }
    if (auto *ArrTy = dyn_cast<ArrayType>(Ty)) {
      return containsI64(ArrTy->getElementType());
    }
    if (auto *VecTy = dyn_cast<VectorType>(Ty)) {
      return containsI64(VecTy->getElementType());
    }
    return false;
  }

  // Auxiliary function: Recursively check if the type contains i64 or a
  // potential pointer to i64
  bool containsSuspiciousPointer(Type *Ty) {
    if (!Ty)
      return false;
    auto It = PlausibleTypes.find(Ty);
    if (It != PlausibleTypes.end())
      return It->second;

    bool Suspicious = false;

    do {
      // Handle pointer type
      if (auto *PTy = dyn_cast<PointerType>(Ty)) {
#if LLVM_VERSION_MAJOR >= 22
        (void)PTy;
        // LLVM 23 uses opaque pointers by default. Conservatively assume a
        // pointer-typed field may eventually reach i64-sized memory.
        // TODO: find user of load i32 %ptr to judge the pointer type.
        Suspicious = true;
        break;
#else
        // Transparent pointer handling logic
        if (PTy->isOpaque()) {
          // Conservatively assume transparent pointers may point to i64
          Suspicious = true;
          break;
        }
        Type *ElemTy = PTy->getNonOpaquePointerElementType();
        // Recursively check non-transparent pointers
        if (ElemTy &&
            (ElemTy->isIntegerTy(64) || containsSuspiciousPointer(ElemTy))) {
          Suspicious = true;
          break;
        }
#endif
      }

      // Recursively check struct members
      if (auto *STy = dyn_cast<StructType>(Ty)) {
        for (Type *ElemTy : STy->elements()) {
          if (containsSuspiciousPointer(ElemTy)) {
            Suspicious = true;
            break;
          }
        }
        break;
      }

      // Handle array/vector types
      if (auto *ArrTy = dyn_cast<ArrayType>(Ty)) {
        if (containsSuspiciousPointer(ArrTy->getElementType())) {
          Suspicious = true;
          break;
        }
      }
      if (auto *VecTy = dyn_cast<VectorType>(Ty)) {
        if (containsSuspiciousPointer(VecTy->getElementType())) {
          Suspicious = true;
          break;
        }
      }
    } while (false);

    PlausibleTypes[Ty] = Suspicious;
    return Suspicious;
  }

private:
  // Those types are considered plausible to be used as thread id or not.
  SmallDenseMap<Type *, bool> PlausibleTypes;
  const Function &F;
  const MemorySSA &MSSA;
  const DominatorTree &DT;
  const LoopInfo &LI;
  const AAResults &AA;
  const DataLayout &DL;
};
} // namespace

namespace __xsan {

AnalysisKey StackObjRaceAnalysis::Key;

bool StackObjRaceResult::mightRace(const Instruction *Mop) const {
  const Value *Addr = extractAddrFromLoadStoreInst(*Mop);
  if (!Addr) {
    llvm_unreachable("StackObjRaceResult::mightRace: not a load/store, "
                     "unexpected instruction");
  }

  auto It = mayRaces.find(Addr);
  if (It == mayRaces.end()) {
    // If the address is not recorded, estimate it is racy conservatively.
    return true;
  }

  // Otherwise, check if the instruction is recorded as racy.
  return It->second.contains(Mop);
}

const SmallPtrSet<const Instruction *, 8> &
StackObjRaceResult::getRacyMOPsForAllocaInst(const AllocaInst &Addr) const {
  auto It = mayRaces.find(&Addr);
  if (It == mayRaces.end()) {
    llvm_unreachable("Invalid address! We have not analyzed it yet.");
  }
  return It->second;
}

StackObjRaceAnalysis::Result
StackObjRaceAnalysis::run(Function &F, FunctionAnalysisManager &FAM) {
  StackObjRaceResult Result;
  auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F).getMSSA();
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &AA = FAM.getResult<AAManager>(F);
  StackRaceAnalyzer SRA(F, MSSA, DT, LI, AA);
  const SmallVector<const AllocaInst *, 16> StackObjects = getStackObjects(F);
  for (const AllocaInst *Alloc : StackObjects) {
    auto RacyMOPs = SRA.allMopsMightRace(*Alloc);
    Result.mayRaces[Alloc] = std::move(RacyMOPs);
  }

  return Result;
}

} // namespace __xsan
