//===------------Instrumentation.h - TransformUtils Infrastructure --------===//
//
// This file is to provide some instrumentation utilities APIs.
// These APIs usually come from the subsequent versions of LLVM, i.e., LLVM 15+.
//
//===----------------------------------------------------------------------===//
#pragma once

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace __xsan {
using namespace llvm;

/// Check if module has flag attached, if not add the flag.
bool checkIfAlreadyInstrumented(Module &M, StringRef Flag);

// Use to ensure the inserted instrumentation has a !nosanitize metadata.
struct InstrumentationIRBuilder : public IRBuilder<> {
private:
  static void ensureNoSanitize(IRBuilder<> &IRB) {
    // Because `AddOrRemoveMetadataToCopy` is private, we can only use it by the
    // delegation of `CollectMetadataToCopy`.
    // Therefore, we create a temporary instruction with the desired !nosanitize
    // metadata.
    static llvm::ReturnInst *I = [&IRB]() {
      auto *I = llvm::ReturnInst::Create(IRB.getContext());
      I->setMetadata(LLVMContext::MD_nosanitize,
                     MDNode::get(I->getContext(), {}));
      return I;
    }();
    // Every instructions created by this IRBuilder will have this metadata.
    IRB.CollectMetadataToCopy(I, {LLVMContext::MD_nosanitize});
    // AddOrRemoveMetadataToCopy(MD_nosanitize, MDNode::get(Context, None));
  }

  static void resetEnsureNoSanitize(IRBuilder<> &IRB) {
    // Because `AddOrRemoveMetadataToCopy` is private, we can only use it by the
    // delegation of `CollectMetadataToCopy`.
    // Therefore, we create a temporary instruction with the desired !nosanitize
    // metadata.
    static llvm::ReturnInst *I = llvm::ReturnInst::Create(IRB.getContext());
    // Every instructions created by this IRBuilder will have this metadata.
    IRB.CollectMetadataToCopy(I, {LLVMContext::MD_nosanitize});
    // AddOrRemoveMetadataToCopy(MD_nosanitize, nullptr);
  }

  struct ScopedNotTagNoSanitize {
    IRBuilder<> &IRB;
    ScopedNotTagNoSanitize(IRBuilder<> &IRB) : IRB(IRB) {
      resetEnsureNoSanitize(IRB);
    }
    ~ScopedNotTagNoSanitize() { ensureNoSanitize(IRB); }
  };

public:
  static void ensureDebugInfo(IRBuilder<> &IRB, const Function &F) {
    if (IRB.getCurrentDebugLocation())
      return;
    if (DISubprogram *SP = F.getSubprogram())
      IRB.SetCurrentDebugLocation(DILocation::get(SP->getContext(), 0, 0, SP));
  }

  static void ensureDebugInfo(IRBuilder<> &IRB, const Instruction &I) {
    if (IRB.getCurrentDebugLocation())
      return;
    IRB.SetCurrentDebugLocation(I.getDebugLoc());
  }

  explicit InstrumentationIRBuilder(LLVMContext &C) : IRBuilder<>(C) {
    ensureNoSanitize(*this);
  }

  explicit InstrumentationIRBuilder(Instruction *IP) : IRBuilder<>(IP) {
    ensureDebugInfo(*this, *IP);
    ensureNoSanitize(*this);
  }
  explicit InstrumentationIRBuilder(IRBuilderBase &IRB)
      : IRBuilder<>(IRB.GetInsertBlock(), IRB.GetInsertPoint()) {
    ensureDebugInfo(*this, *IRB.GetInsertPoint());
    ensureNoSanitize(*this);
  }

  InstrumentationIRBuilder(BasicBlock *TheBB, BasicBlock::iterator IP)
      : IRBuilder<>(TheBB, IP) {
    ensureDebugInfo(*this, *TheBB->getParent());
    ensureNoSanitize(*this);
  }

  ScopedNotTagNoSanitize scopedNotTagNoSanitize() {
    return ScopedNotTagNoSanitize(*this);
  }
};

/// Add nounwind attribute automatically
struct NoUnwindModuleWrapper {
  Module &M;
  const AttributeList Attr;
  NoUnwindModuleWrapper(Module &M)
      : M(M),
        Attr(AttributeList::get(M.getContext(), AttributeList::FunctionIndex,
                                Attribute::NoUnwind)) {}

  operator Module &() { return M; }

  FunctionCallee getOrInsertFunction(StringRef Name, FunctionType *T,
                                     AttributeList AttributeList) {
    const auto Attr =
        AttributeList::get(M.getContext(), {AttributeList, this->Attr});
    return M.getOrInsertFunction(Name, T, Attr);
  }

  FunctionCallee getOrInsertFunction(StringRef Name, FunctionType *T) {
    return M.getOrInsertFunction(Name, T, Attr);
  }

  template <typename... ArgsTy>
  FunctionCallee getOrInsertFunction(StringRef Name,
                                     AttributeList AttributeList, Type *RetTy,
                                     ArgsTy... Args) {
    const auto Attr =
        AttributeList::get(M.getContext(), {AttributeList, this->Attr});
    return M.getOrInsertFunction(Name, Attr, RetTy, Args...);
  }

  template <typename... ArgsTy>
  FunctionCallee getOrInsertFunction(StringRef Name, Type *RetTy,
                                     ArgsTy... Args) {
    return M.getOrInsertFunction(Name, Attr, RetTy, Args...);
  }

  template <typename... ArgsTy>
  FunctionCallee
  getOrInsertFunction(StringRef Name, AttributeList AttributeList,
                      FunctionType *Invalid, ArgsTy... Args) = delete;
};

/// Utils class to check if a value is loop invariant.
/// Note that this class should be used at those simple loops.
class LoopInvariantChecker {
public:
  LoopInvariantChecker(const DominatorTree &DT, AliasAnalysis &AA);
  bool hasUBSan() const { return UBSanExists; }
  bool isLoopInvariant(Value *V, const Loop *L);
  bool isLoopInvariant(ScalarEvolution &SE, const SCEV *S, const Loop *L);

private:
  bool isLoopInvariant(const SCEV *S, const Loop *L);
  bool isLoadLoopInvariant(const LoadInst *LI, const Loop *L);
  SmallVectorImpl<MemoryLocation> &getStoresInLoopLazily(const Loop *L);

  bool UBSanExists;
  const DominatorTree &DT;
  AAResults &AA;
  DenseMap<const Loop *, SmallVector<MemoryLocation, 8>> StoresInLoop;
};

enum class LoopOptLeval {
  NoOpt,                   /* No optimization */
  RelocateInvariantChecks, /* Relocate invariant checks */
  CombineToRangeCheck,     /* Combine periodic checks to range checks*/
  CombinePeriodicChecks,   /* Combine periodic checks, including range checks */
  Full                     /* RelocateInvariantChecks + CombinePeriodicChecks*/
};

/*
 Optimize in simple canonical loop, i.e., no branching, no function call:
  1. Relocate invariant checks: if a loop invariant is used in the loop, sink
      it out of the loop.
  2. Combine preiodic checks: combine temporal adjacent periodic checks into a
      single check.
*/
class LoopMopInstrumenter {
private:
  using DupVec = SmallVector<Instruction *, 4>;
  struct LoopMop {
    Instruction *Mop;
    Value *Address;
    Loop *Loop;
    size_t MopSize;
    /// Records the MOPs redundant to this MOPs.
    DupVec DupTo;
    /* If true, the Mop might not be executed in every iteration of the loop. */
    bool InBranch;
    bool IsWrite;
  };

public:
  static LoopMopInstrumenter create(Function &F, FunctionAnalysisManager &FAM,
                                    LoopOptLeval OptLevel);

  /*
   1. Relocate invariant checks: if a loop invariant is used in the loop, sink
      it out of the loop.
   2. Combine preiodic checks: combine temporal adjacent periodic checks into a
      single check.
   */
  void instrument();

private:
  LoopMopInstrumenter(Function &F, FunctionAnalysisManager &FAM,
                      LoopOptLeval OptLevel);

  // A simple loop is currently defined as a loop with
  // 1. single header, exit, latch, exiting. predecessor.
  // 2. conatains no atomic instructions and
  //    no function calls (apart from pure readonly function)
  /// TODO: for ASan, such restrictions can be relaxed.
  /// TODO: if the MOP is ahead of all exiting, multiple exitings
  ///       do not affect the loop optimization.
  /// TOOD: migrate to LLVM16+, which introduced attribute `memory(...)`
  ///       and implemented more precise memory description.
  bool isSimpleLoop(const Loop *L);

  /// Filter out those obvious duplicate MOPs in the same BB,
  /// being formalized as follows
  /// For ∀m1 ≠ m2 ∈ MOPs, m1 is a duplicate of m2 if
  ///     1. m1.addr = m2.addr
  ///     2. m1.type = m2.type ∨ m1.type = read
  ///     3. ∀ i ∈ (m1.loc, m2.loc), isNotCall(i)
  /// `collectLoopMopCandidates` is the caller of this function,
  ///  guaranting that the third condition holding.
  void filterAndAddMops(SmallVectorImpl<LoopMop> &MOPs);
  // Collect loop mop candidates, i.e., MOPs that in SIMPLE loops.
  // Those MOPs in the same BB are guaranteed to be adjacent and ordered by
  // their IR order.
  void collectLoopMopCandidates();
  SmallVectorImpl<LoopMop> &getLoopMopCandidates();
  // hoist / sink the checks if their checked addresses are loop invariant
  bool relocateInvariantChecks();
  // Induction-based Instrumentation
  bool combinePeriodicChecks(bool RangeAccessOnly = true);
  // Tag the MOP and the relevant dup MOPs with !xsan.delegated metadata.
  // Return the number of dup MOPs.
  unsigned tagMopAsDelegated(LoopMop &Mop);

  LoopOptLeval OptLevel;
  Function &F;
  FunctionAnalysisManager &FAM;
  LoopInfo &LI;
  MemorySSAUpdater MSSAU;
  AAResults &AA;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  const DataLayout &DL;

  SmallPtrSet<const Loop *, 16> SimpleLoops;
  SmallPtrSet<const Loop *, 16> ComplexLoops;
  SmallVector<LoopMop, 16> LoopMopCandidates;
  bool MopCollected;

  FunctionCallee XsanRangeRead;
  FunctionCallee XsanRangeWrite;

  // Accesses sizes are powers of two: 1, 2, 4, 8, 16.
  static const size_t kNumberOfAccessSizes = 5;
  FunctionCallee XsanPeriodRead[kNumberOfAccessSizes];
  FunctionCallee XsanPeriodWrite[kNumberOfAccessSizes];
  FunctionCallee XsanRead[kNumberOfAccessSizes];
  FunctionCallee XsanWrite[kNumberOfAccessSizes];
  LoopInvariantChecker LIC;
};

} // namespace __xsan
