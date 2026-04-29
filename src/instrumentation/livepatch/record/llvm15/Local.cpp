#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumeBundleQueries.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/EHPersonalities.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <cassert>

#include "PatchCore.h"
#include "livepatch/record/SanitizingRecorder.h"

using namespace llvm;

namespace __xsan::desan {
namespace {

BasicBlock *changeToInvokeAndSplitBasicBlock(CallInst *CI,
                                             BasicBlock *UnwindEdge,
                                             DomTreeUpdater *DTU);

static __xsan::ManualInterception ChangeToInvokeAndSplitBasicBlockInterceptee(
    &changeToInvokeAndSplitBasicBlock, &llvm::changeToInvokeAndSplitBasicBlock,
    InterceptionTargetProcesses, InterceptionGroup);

// This function use `Split->getInstList().pop_front();` to remove the
// instruction. But some interceptors need to record the delete event,
// so we need to intercept and use `eraseFromParent` instead.
//
// Note: LLVM23 has already changed to use `eraseFromParent`, so we only need to
// intercept LLVM15.
BasicBlock *changeToInvokeAndSplitBasicBlock(CallInst *CI,
                                             BasicBlock *UnwindEdge,
                                             DomTreeUpdater *DTU) {
  BasicBlock *BB = CI->getParent();

  // Convert this function call into an invoke instruction.  First, split the
  // basic block.
  BasicBlock *Split = SplitBlock(BB, CI, DTU, /*LI=*/nullptr, /*MSSAU*/ nullptr,
                                 CI->getName() + ".noexc");

  // Delete the unconditional branch inserted by SplitBlock
  BB->getInstList().pop_back();

  // Create the new invoke instruction.
  SmallVector<Value *, 8> InvokeArgs(CI->args());
  SmallVector<OperandBundleDef, 1> OpBundles;

  CI->getOperandBundlesAsDefs(OpBundles);

  // Note: we're round tripping operand bundles through memory here, and that
  // can potentially be avoided with a cleverer API design that we do not have
  // as of this time.

  InvokeInst *II =
      InvokeInst::Create(CI->getFunctionType(), CI->getCalledOperand(), Split,
                         UnwindEdge, InvokeArgs, OpBundles, CI->getName(), BB);
  II->setDebugLoc(CI->getDebugLoc());
  II->setCallingConv(CI->getCallingConv());
  II->setAttributes(CI->getAttributes());
  II->setMetadata(LLVMContext::MD_prof, CI->getMetadata(LLVMContext::MD_prof));

  if (DTU)
    DTU->applyUpdates({{DominatorTree::Insert, BB, UnwindEdge}});

  // Make sure that anything using the call now uses the invoke!  This also
  // updates the CallGraph if present, because it uses a WeakTrackingVH.
  CI->replaceAllUsesWith(II);

  // Delete the original call
  CI->eraseFromParent();
  return Split;
}

} // namespace
} // namespace __xsan::desan
