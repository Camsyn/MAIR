/**
 This Pass is only used for testing purposes.
 We can use this pass to test LLVM functionality and ensure that the pass is
 working as expected.
 */

#include "PassRegistry.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasAnalysisEvaluator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/StackSafetyAnalysis.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <cstddef>

#include "Analysis/ActiveMopAnalysis.h"
#include "Analysis/MopRecurrenceReducer.h"
#include "PassRegistry.h"

using namespace llvm;

namespace __xsan {

class TestPass : public llvm::PassInfoMixin<TestPass> {
public:
  TestPass() {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

void Test(Function &F, FunctionAnalysisManager &AM);


static void runInternal(Function &F, AAResults &AA) {
  if (!F.getName().contains("foo")) return;
  const DataLayout &DL = F.getParent()->getDataLayout();

  SetVector<std::pair<const Value *, Type *>> Pointers;
  SmallSetVector<CallBase *, 16> Calls;
  SetVector<Value *> Loads;
  SetVector<Value *> Stores;

  bool load = false, store = false;
  for (Instruction &Inst : instructions(F)) {
    if (auto *Call = dyn_cast<CallBase>(&Inst)) {
      Pointers.insert({Call, Call->getType()});
    }
    if (auto *LI = dyn_cast<LoadInst>(&Inst)) {
      if (load) { continue; }
      Pointers.insert({LI->getPointerOperand(), LI->getType()});
      Loads.insert(LI);
      load = true;
    } else if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
      if (store) { continue; }
      if (SI->getValueOperand()->getType()->isPointerTy()) {
        continue;
      }
      Pointers.insert({SI->getPointerOperand(),
                       SI->getValueOperand()->getType()});
      Stores.insert(SI);
      store = true;
    }
  }
  // for (Instruction &Inst : instructions(F)) {
  //   if (auto *LI = dyn_cast<LoadInst>(&Inst)) {
  //     Pointers.insert({LI->getPointerOperand(), LI->getType()});
  //     Loads.insert(LI);
  //   } else if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
  //     Pointers.insert({SI->getPointerOperand(),
  //                      SI->getValueOperand()->getType()});
  //     Stores.insert(SI);
  //   } else if (auto *CB = dyn_cast<CallBase>(&Inst))
  //     Calls.insert(CB);
  // }


  // iterate over the worklist, and run the full (n^2)/2 disambiguations
  for (auto I1 = Pointers.begin(), E = Pointers.end(); I1 != E; ++I1) {
    LocationSize Size1 = LocationSize::precise(DL.getTypeStoreSize(I1->second));
    for (auto I2 = Pointers.begin(); I2 != I1; ++I2) {
      LocationSize Size2 =
          LocationSize::precise(DL.getTypeStoreSize(I2->second));
      AliasResult AR = AA.alias(I1->first, Size1, I2->first, Size2);
      switch (AR) {
      case AliasResult::NoAlias:
        errs() << "NoAlias: \n\t" << *I1->first << "\n\t" << *I2->first << "\n";
        break;
      case AliasResult::MayAlias:
        errs() << "MayAlias: \n\t" << *I1->first << "\n\t" << *I2->first << "\n";
        break;
      case AliasResult::PartialAlias:
        errs() << "PartialAlias: \n\t" << *I1->first << "\n\t" << *I2->first << "\n";
        break;
      case AliasResult::MustAlias:
        errs() << "MustAlias: \n\t" << *I1->first << "\n\t" << *I2->first << "\n";
        break;
      }
    }
  }
}

static void testAA(Module &M, ModuleAnalysisManager &MAM) {
  AAEvaluator AAE;
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (auto &F : M) {
    if (F.isDeclaration()) continue;
    // AAE.run(F, FAM);
    auto &AA = FAM.getResult<AAManager>(F);
    runInternal(F, AA);
  }
}

static bool isLoopCountingPhi(PHINode *PN) {
  // 检查是否正好有两个 incoming edge
  if (PN->getNumIncomingValues() != 2)
    return false;

  // 检查是否是 interger
  if (!PN->getType()->isIntegerTy())
    return false;

  bool hasZero = false;
  bool hasAddUp = false;
  // 遍历 PHI 节点的所有 incoming value
  for (unsigned idx = 0; idx < PN->getNumIncomingValues(); idx++) {
    Value *incoming = PN->getIncomingValue(idx);
    // 如果 incoming 为常数，检查是否为 0
    if (ConstantInt *CI = dyn_cast<ConstantInt>(incoming)) {
      if (CI->isZero()) {
        hasZero = true;
        continue;
      }
    }
    // 否则，若 incoming 来自一个加法指令，则检查加法指令是否为 %phi + 1
    if (Instruction *IncomingInst = dyn_cast<Instruction>(incoming)) {
      if (IncomingInst->getOpcode() == Instruction::Add) {
        // 检查是否有一个操作数是当前的 PHI 节点，另一个操作数为常数 1
        Value *Op0 = IncomingInst->getOperand(0);
        Value *Op1 = IncomingInst->getOperand(1);
        if ((Op0 == PN && isa<ConstantInt>(Op1) &&
             cast<ConstantInt>(Op1)->equalsInt(1)) ||
            (Op1 == PN && isa<ConstantInt>(Op0) &&
             cast<ConstantInt>(Op0)->equalsInt(1))) {
          hasAddUp = true;
        }
      }
    }
  }
  return hasZero && hasAddUp;
}

/*
 目前仅关注简单的循环：
 1. 单 header， 单 preheader, 单 latch, 单 exit
*/
static Value *getOrInsertLoopCounter(Loop *L) {
  // 1. 尝试在循环头（header）中查找已有的循环计数器
  BasicBlock *Header = L->getHeader(),
             *Preheader = L->getLoopPreheader(),
             *Latch = L->getLoopLatch(),
             *Exit = L->getExitBlock();
  if (!Header || !Preheader || !Latch || !Exit)
    return nullptr; // 若循环没有 header，则无法安全插桩

  // 若 header存在 > 2 个 predecessor，则无法安全插桩
  if (!Header->hasNPredecessors(2))
    return nullptr;

  // 在 header 的第一个非 Phi 插入点前创建新的 Phi 节点
  IRBuilder<> IRB(&*Header->getFirstInsertionPt());
  for (Instruction &I : *Header) {
    // 只考虑 PHI 节点
    PHINode *Counter = dyn_cast<PHINode>(&I);
    if (!Counter)
      continue;

    if (!isLoopCountingPhi(Counter)) {
      continue;
    }
    
    return Counter;
  }
  // 2. 未找到已有计数器，则构造新的计数器（纯 Phi 版本，不使用 Alloca）
  LLVMContext &Ctx = Header->getContext();

  // 预设 incoming 数量：1 来自 preheader，加上每个 latch 块一个
  SmallVector<BasicBlock*, 4> Latches;
  L->getLoopLatches(Latches);
  unsigned NumIncoming = 1 + Latches.size();
  PHINode *CounterPhi =
      IRB.CreatePHI(IRB.getInt64Ty(), NumIncoming, "loop.count");

  // 来自 preheader 的 incoming 值为 0
  CounterPhi->addIncoming(IRB.getInt64(0), Preheader);
  // 对于每个 latch 块，在其出口处插入加法指令，并将结果加入 Phi 节点
  for (BasicBlock *Latch : Latches) {
    IRBuilder<> LatchBuilder(Latch->getTerminator());
    // 生成：%inc = add counterPhi, 1
    Value *Incr =
        LatchBuilder.CreateAdd(CounterPhi, IRB.getInt64(1), "inc.loop.count");
    CounterPhi->addIncoming(Incr, Latch);
  }
  return CounterPhi;
}

void testSCEV(Function &F, FunctionAnalysisManager &FAM) {
  if (F.isDeclaration() || F.empty())
    return;
  // 获取 SCEV 分析结果
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
  const auto &DL = F.getParent()->getDataLayout();
  for (auto &BB : F) {
    for (auto &Inst : BB) {
      Value *Addr = nullptr;
      size_t MopSize = 0;
      bool IsWrite = false;
      if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
        Addr = SI->getPointerOperand();
        MopSize = DL.getTypeStoreSize(SI->getValueOperand()->getType());
        IsWrite = true;
      } else if (auto *LI = dyn_cast<LoadInst>(&Inst)) {
        Addr = LI->getPointerOperand();
        MopSize = DL.getTypeStoreSize(LI->getType());
        IsWrite = false;
      }

      if (!Addr)
        continue;
      auto *PtrSCEV = SE.getSCEV(Addr);
      if (!PtrSCEV)
        continue;

      errs() << *PtrSCEV << "\n";
      /// TODO: transfer SCEVAddExpr(SCEVAddRecExpr) to SCEVAddRecExpr
      const auto *AR = dyn_cast<SCEVAddRecExpr>(PtrSCEV);
      if (!AR)
        continue;
      errs() << "Load/Store: " << Inst << "\n";
      errs() << "\tAddr: " << *Addr << "\n";
      errs() << "\tSCEV: " << *AR << "\n";
      const Loop *L = AR->getLoop();

      const auto *Step  = AR->getStepRecurrence(SE);

      // ----------- 提取 Loop-Invariant 步长 -----------
      const SCEVConstant *ConstStep = dyn_cast<SCEVConstant>(Step);
      if (!ConstStep) {
        // 如果不是常量，检查是否为 Loop-Invariant
        if (SE.isLoopInvariant(Step, L)) {
          errs() << "Step is loop-invariant but not constant: " << *Step << "\n";
        } else {
          errs() << "Step is variant inside loop: " << *Step << "\n";
          continue;  // 跳过不是循环不变量的情况
        }
      }
      ConstantInt *StepVal = ConstStep->getValue();

      auto *LoopLatch = L->getLoopLatch();
      if (!LoopLatch || !DT.dominates(Inst.getParent(), LoopLatch)) {
        // 如果 Inst 不支配 循环回归块，说明其在循环内部分支中，不进行分析
        continue;
      }

      auto *ExitBlock = L->getUniqueExitBlock();
      if (!ExitBlock || !PDT.dominates(ExitBlock, LoopLatch)) {
        continue;
      }

      IRBuilder<> IRB(&*ExitBlock->getFirstInsertionPt());

      // 你可以提取起始值、步长等
#if LLVM_VERSION_MAJOR >= 22
      SCEVExpander Expander(SE, "expander");
#else
      SCEVExpander Expander(SE, DL, "expander");
#endif

      auto *StartSCEV = AR->getStart();
      Value *Start = Expander.expandCodeFor(StartSCEV, StartSCEV->getType(),
                                            &*IRB.GetInsertPoint());

      // ----------- 提取真实的 Value -----------

      auto *PrintFunc = F.getParent()->getFunction("print");
      Instruction *LastAddr = dyn_cast<Instruction>(Addr);
      assert(LastAddr && "LastAddr is null");

      /// If LastAddr does not dominate ExitBlock, the value should be calculated in
      /// place after the loop.
      if (!DT.dominates(LastAddr->getParent(), ExitBlock)) {
        Value *Counter = getOrInsertLoopCounter(const_cast<Loop *>(L));
        if (!Counter) {
          continue;
        }

        // If not i64, cast it to i64
        if (Counter->getType() != IRB.getInt64Ty()) {
          Counter = IRB.CreateZExt(Counter, IRB.getInt64Ty());
        }

        // 根据 Counter 和 StepVal 计算出实际地址
        // LastAddr = BaseValue + Counter * StepVal
        // 使用 GEP 指令来计算地址
        LastAddr = (Instruction *)IRB.CreateGEP(IRB.getInt8Ty(), Start,
                                                IRB.CreateMul(Counter, StepVal));
      }

      if (SE.hasLoopInvariantBackedgeTakenCount(L)) {
        errs() << "Loop has backedge-taken count: " << *SE.getBackedgeTakenCount(L) << "\n";
      }
      auto *BackEgdeTakenCount =  SE.getBackedgeTakenCount(L);

      // 打印查看
      errs() << "---------\n";
      errs() << "Pointer SCEV is an AddRecExpr:\n";
      errs() << "  Load/Store: " << Inst << "\n";
      errs() << "  Addr: " << *Addr << "\n";
      errs() << "  SCEV: " << *AR << "\n";
      errs() << "  Loop: " << L->getName() << "\n"; // 可能为空
      errs() << "  Start: " << *Start << "\n";
      errs() << "  Step: " << *StepVal << "\n";
      errs() << "  BackedgeTakenCount: " << *BackEgdeTakenCount << "\n";
      errs() << "---------\n";

      // void print(char *beg, char *end, unsigned int step, unsigned int mopSize)
      IRB.CreateCall(PrintFunc,
                     {Start, LastAddr, StepVal, IRB.getInt32(MopSize)});

      // // 也可做更多分析，比如判断 Step 是否为常量等
      // if (const SCEVConstant *CStep = dyn_cast<SCEVConstant>(Step)) {
      //   APInt StepVal = CStep->getAPInt();
      //   // 在此处可以根据步长做进一步判断，比如如果步长是
      //   // 4，可能说明每次迭代移动 4 bytes。
      //   }
    }
  }
}

PreservedAnalyses TestPass::run(Module &M, ModuleAnalysisManager &MAM) {
  bool Modified = false;
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  
  // FAM.registerPass([]() { return DominatorTreeAnalysis(); });
  errs() << "TestPass run\n";
  // testAA(M, MAM);
  for (auto &F : M) {
    if (F.isDeclaration()) continue;
    errs() << "Function: " << F.getName() << "\n";
    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
    if (LI.empty())
      continue;
    for (Loop *L : LI) {
      errs() << "Loop: " << L->getName() << "\n";
      for (BasicBlock *BB : L->blocks()) {
        for (Instruction &Inst : *BB) {
          Value *Addr = nullptr;
          if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
            Addr = SI->getPointerOperand();
          } else if (auto *LI = dyn_cast<LoadInst>(&Inst)) {
            Addr = LI->getPointerOperand();
          }
          if (!Addr) {
            continue;
          }
          errs() << "Inst: " << Inst << "\n";
          errs() << "\tAddr: " << *Addr << "\n";
          errs() << "\tIsLoopInvariant: " << L->isLoopInvariant(Addr) << "\n";
        }
      }
    }
    // testSCEV(F, FAM);
  }
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}


} // namespace __xsan

void registerTestPassForClangAndOpt(PassBuilder &PB) {

  PB.registerOptimizerLastEPCallback(
#if LLVM_VERSION_MAJOR >= 22
      [=](ModulePassManager &MPM, OptimizationLevel Level, ThinOrFullLTOPhase) {
#else
      [=](ModulePassManager &MPM, OptimizationLevel Level) {
#endif
        MPM.addPass(__xsan::TestPass());
      });

  // 这里注册opt回调的名称
  PB.registerPipelineParsingCallback(
      [=](StringRef Name, ModulePassManager &MPM,
          ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "test") {
          MPM.addPass(__xsan::TestPass());
          return true;
        }
        return false;
      });
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "XSan Test Pass", LLVM_VERSION_STRING,
          registerTestPassForClangAndOpt};
}
