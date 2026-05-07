#include "UbsanInstTagging.hpp"
#include "Utils/MetaDataUtils.h"
#include "Utils/UbsanUtils.h"
#include "Utils/ValueUtils.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

using namespace llvm;

UbsanInstTaggingPass::UbsanInstTaggingPass() {}

namespace {

// When `action` return false, the instruction will be ignored.
void walkOverDefUseChain(ArrayRef<Instruction *> Insts,
                         function_ref<bool(llvm::Instruction &)> action) {
  SmallPtrSet<Instruction *, 4> Visited;
  SmallVector<Instruction *, 4> Worklist(Insts.begin(), Insts.end());

  // DFS
  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();

    if (!Visited.insert(I).second)
      continue;

    if (!action(*I)) {
      continue;
    }

    // def -> use
    for (Use &U : I->uses()) {
      Instruction *User = dyn_cast<Instruction>(U.getUser());
      if (!User) {
        continue;
      }

      Worklist.push_back(User);
    }
  }
}

// When `action` return false, the instruction will be ignored.
void walkOverUseDefChain(Instruction &I,
                         function_ref<bool(llvm::Instruction &)> action) {
  SmallPtrSet<Instruction *, 4> Visited;
  SmallVector<Instruction *, 4> Worklist;

  Worklist.push_back(&I);

  // DFS
  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();

    if (!Visited.insert(I).second)
      continue;

    if (!action(*I)) {
      continue;
    }

    // use -> def
    for (Value *Op : I->operands()) {
      Instruction *OpInst = dyn_cast<Instruction>(Op);
      if (!OpInst) {
        continue;
      }
      Worklist.push_back(OpInst);
    }
  }
}

} // namespace

PreservedAnalyses UbsanInstTaggingPass::run(Module &M,
                                            ModuleAnalysisManager &_) {
  // If any function is UBSan function, we need to tag the module.
  // Otherwise, we can skip the module.
  if (!__xsan::hasUbsanFunction(M)) {
    return PreservedAnalyses::all();
  }

  SmallVector<Instruction *, 4> UbsanBoundary;
  auto CollectBoundaryInst = [&](Instruction &I) -> bool {
    if (!__xsan::isNoSanitize(I)) {
      return false;
    }

    for (Value *Op : I.operands()) {
      Instruction *OpInst = dyn_cast<Instruction>(Op);
      if (OpInst && __xsan::isNoSanitize(*OpInst)) {
        return true;
      }
    }

    // If all of the inst operands are not nosanitize, the instruction is a
    // boundary.
    UbsanBoundary.push_back(&I);
    return true;
  };

  auto MarkUbsan = [&](Instruction &I) {
    if (!__xsan::isNoSanitize(I)) {
      return false;
    }
    __xsan::UBSanInst::set(I);
    return true;
  };

  for (auto &F : M) {
    for (auto &BB : F) {
      if (!__xsan::isUbsanFallbackBlock(BB))
        continue;
      for (auto &I : BB)
        __xsan::UBSanInst::set(I);
      auto *Pred = BB.getSinglePredecessor();
      assert(Pred && "UBSan fallback block should have only one predecessor");
      // Find the branch instruction in the predecessor block.
      auto *Br = dyn_cast<BranchInst>(Pred->getTerminator());
      if (!Br || !__xsan::isNoSanitize(*Br)) {
        // The UBSan's check might be eliminated by the optimization.
        // Some UBSan's fallback blocks are not guarded by a UBSan's check.
        continue;
      }
      // Backward tracking
      walkOverUseDefChain(*Br, CollectBoundaryInst);
      // Forward tracking
      walkOverDefUseChain(UbsanBoundary, MarkUbsan);
      UbsanBoundary.clear();
    }
  }

  return PreservedAnalyses::all();
}
