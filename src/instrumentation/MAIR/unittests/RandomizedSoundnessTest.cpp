// Executes random control-flow graphs and checks what the optimized MAIR graph
// would observe against what the function accesses.
//
// A generated function stores through two fixed pointers and through %v, a
// pointer that a call in one block defines anew each time it runs. Blocks
// branch to their successor in layout order and to one random block, so the
// graphs contain natural loops, nested loops and irreducible cycles. A random
// walk then records, in execution order, every store, every barrier call and
// every observation the graph keeps, each with the address it has at that
// moment. Two properties must hold for a walk that reaches the return:
//
//   - every store is matched by an observation of the same address with no
//     barrier executed between the two, and
//   - every observation is matched by such a store.
//
// The first catches an observation deleted or moved without a valid witness,
// the second one that was moved to where the program does not access.

#include "MAIR/Transforms/CommonObservationMotionPass.h"
#include "MAIR/Transforms/LoopInvariantObservationMotionPass.h"
#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Transforms/SimplifyGraphPass.h"
#include "MAIRPassTestUtil.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

#include <random>

using namespace mair;

namespace {

struct BarrierCallIsBarrier {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
    return Call && Call->getCalledFunction() &&
           Call->getCalledFunction()->getName() == "barrier";
  }
  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return isForwardBarrier(I);
  }
};

enum class Pipeline { Eliminate, CommonMotion, LoopMotion };

/// Returns the call that defines the variant pointer.
llvm::Instruction *generateFunction(llvm::Module &M, std::mt19937 &Rng,
                                    bool WithBarriers) {
  llvm::LLVMContext &Ctx = M.getContext();
  llvm::Type *PtrTy = llvm::PointerType::getUnqual(Ctx);
  llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
  const unsigned NumBlocks = 3 + Rng() % 6;

  std::vector<llvm::Type *> Params{PtrTy, PtrTy};
  Params.insert(Params.end(), NumBlocks, llvm::Type::getInt1Ty(Ctx));
  llvm::Function *F =
      llvm::Function::Create(llvm::FunctionType::get(VoidTy, Params, false),
                             llvm::GlobalValue::ExternalLinkage, "f", M);
  llvm::FunctionCallee Next =
      M.getOrInsertFunction("next", llvm::FunctionType::get(PtrTy, false));
  llvm::FunctionCallee Barrier =
      M.getOrInsertFunction("barrier", llvm::FunctionType::get(VoidTy, false));

  std::vector<llvm::BasicBlock *> Blocks;
  for (unsigned I = 0; I < NumBlocks; ++I)
    Blocks.push_back(llvm::BasicBlock::Create(Ctx, "b" + llvm::Twine(I), F));
  // The layout successor keeps every block reachable and able to return. The
  // other successor is any block but the entry, which adds backedges and
  // jumps into the middle of cycles.
  for (unsigned I = 0; I + 1 < NumBlocks; ++I) {
    llvm::IRBuilder<> Builder(Blocks[I]);
    const unsigned Other = 1 + Rng() % (NumBlocks - 1);
    if (Other == I + 1)
      Builder.CreateBr(Blocks[I + 1]);
    else
      Builder.CreateCondBr(F->getArg(2 + I), Blocks[I + 1], Blocks[Other]);
  }
  llvm::IRBuilder<>(Blocks.back()).CreateRetVoid();

  llvm::BasicBlock *DefBlock = Blocks[Rng() % NumBlocks];
  llvm::Instruction *Variant =
      llvm::CallInst::Create(Next, "v", DefBlock->getFirstInsertionPt());
  llvm::DominatorTree DT(*F);
  llvm::Value *Zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0);
  for (llvm::BasicBlock *BB : Blocks) {
    llvm::IRBuilder<> Builder(BB->getTerminator());
    for (unsigned Items = Rng() % 4; Items; --Items) {
      switch (Rng() % (WithBarriers ? 5 : 4)) {
      case 0:
        Builder.CreateStore(Zero, F->getArg(0));
        break;
      case 1:
        Builder.CreateStore(Zero, F->getArg(1));
        break;
      case 2:
      case 3:
        Builder.CreateStore(Zero, DT.dominates(DefBlock, BB)
                                      ? static_cast<llvm::Value *>(Variant)
                                      : F->getArg(0));
        break;
      default:
        Builder.CreateCall(Barrier);
      }
    }
  }
  return Variant;
}

struct KeptObservation {
  const llvm::Value *Base = nullptr;
  const llvm::Instruction *Anchor = nullptr;
  const llvm::BasicBlock *EdgeFrom = nullptr;
  const llvm::BasicBlock *EdgeTo = nullptr;
  const AnyProvenanceExecutedGuard *Guard = nullptr;
};

struct Event {
  unsigned Time;
  const llvm::Value *Base;
  /// How often the variant pointer had been defined; zero for other bases.
  unsigned Version;
};

/// Empty if the walk satisfies both properties or does not return.
std::string checkOneWalk(const llvm::Function &F,
                         llvm::ArrayRef<KeptObservation> Kept,
                         const llvm::Instruction *Variant, std::mt19937 &Rng) {
  llvm::SmallVector<Event, 32> Accesses, Observations;
  llvm::SmallVector<unsigned, 8> Barriers;
  llvm::DenseMap<const AnyProvenanceExecutedGuard *, bool> Executed;
  unsigned Time = 0, Version = 0;
  auto Fire = [&](const KeptObservation &Obs) {
    if (Obs.Guard && !Executed.lookup(Obs.Guard))
      return;
    Observations.push_back(
        {Time++, Obs.Base, Obs.Base == Variant ? Version : 0});
  };

  const llvm::BasicBlock *BB = &F.getEntryBlock();
  for (unsigned Steps = 0;; ++Steps) {
    if (Steps == 64)
      return "";
    for (const llvm::Instruction &I : *BB) {
      // Instrumentation is inserted in front of its anchor.
      for (const KeptObservation &Obs : Kept)
        if (!Obs.EdgeFrom && Obs.Anchor == &I)
          Fire(Obs);
      if (&I == Variant)
        ++Version;
      else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
        Accesses.push_back(
            {Time++, Store->getPointerOperand(),
             Store->getPointerOperand() == Variant ? Version : 0});
      else if (BarrierCallIsBarrier::isForwardBarrier(I))
        Barriers.push_back(Time++);
    }
    // A guard is true once a provenance block has run since the reset block.
    for (const KeptObservation &Obs : Kept) {
      if (!Obs.Guard)
        continue;
      if (Obs.Guard->getResetBlock() == BB)
        Executed[Obs.Guard] = false;
      for (const llvm::Instruction *Provenance : Obs.Guard->getProvenance())
        if (Provenance->getParent() == BB)
          Executed[Obs.Guard] = true;
    }
    const llvm::Instruction *Terminator = BB->getTerminator();
    if (Terminator->getNumSuccessors() == 0)
      break;
    const llvm::BasicBlock *Next =
        Terminator->getSuccessor(Rng() % Terminator->getNumSuccessors());
    for (const KeptObservation &Obs : Kept)
      if (Obs.EdgeFrom == BB && Obs.EdgeTo == Next)
        Fire(Obs);
    BB = Next;
  }

  auto Matches = [&](const Event &A, const Event &B) {
    if (A.Base != B.Base || A.Version != B.Version)
      return false;
    const unsigned Low = std::min(A.Time, B.Time);
    const unsigned High = std::max(A.Time, B.Time);
    return llvm::none_of(Barriers, [&](unsigned Barrier) {
      return Low < Barrier && Barrier < High;
    });
  };
  auto Describe = [&](const Event &E) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    E.Base->printAsOperand(OS, false);
    OS << " (definition " << E.Version << ") at time " << E.Time;
    return Text;
  };
  for (const Event &Access : Accesses)
    if (llvm::none_of(Observations,
                      [&](const Event &Obs) { return Matches(Access, Obs); }))
      return "unobserved store to " + Describe(Access);
  for (const Event &Obs : Observations)
    if (llvm::none_of(Accesses, [&](const Event &Access) {
          return Matches(Access, Obs);
        }))
      return "observation without a store of " + Describe(Obs);
  return "";
}

void checkRandomFunctions(Pipeline Passes, bool WithBarriers,
                          MAIROptimizationConfig Config, unsigned NumSeeds) {
  for (unsigned Seed = 0; Seed < NumSeeds; ++Seed) {
    llvm::LLVMContext Ctx;
    llvm::Module M("random", Ctx);
    std::mt19937 Rng(Seed);
    llvm::Instruction *Variant = generateFunction(M, Rng, WithBarriers);
    llvm::Function &F = *Variant->getFunction();
    ASSERT_FALSE(llvm::verifyFunction(F, &llvm::errs()));

    llvm::PassInstrumentationCallbacks PIC;
    llvm::FunctionAnalysisManager FAM;
    registerLoopAnalyses(FAM, PIC);
    MAIRGraph Graph =
        createGraphWithFunctionAnalyses<BarrierCallIsBarrier>(F, FAM, Config);
    MAIRGraphAnalysisManager AM;
    registerMAIRGraphAnalyses(AM, &PIC, &FAM);
    runPassOnGraphTree(SimplifyGraphPass(), Graph, AM);
    runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
    if (Passes == Pipeline::LoopMotion) {
      runPassOnGraphTree(LoopInvariantObservationMotionPass(), Graph, AM);
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
    }
    if (Passes != Pipeline::Eliminate) {
      runPassOnGraphTree(CommonObservationMotionPass(), Graph, AM);
      runPassOnGraphTree(RedundantObservationEliminationPass(), Graph, AM);
      runPassOnGraphTree(SimplifyGraphPass(), Graph, AM);
    }
    ASSERT_TRUE(Graph.verify(&llvm::errs())) << "seed " << Seed;

    llvm::SmallVector<KeptObservation, 16> Kept;
    for (const ProgramPoint &PP : testProgramPoints(Graph))
      for (const Observation &Obs : PP)
        Kept.push_back(
            {Obs.getBase(), &PP.getAnchor(), PP.getEdgeFrom(), PP.getEdgeTo(),
             std::get_if<AnyProvenanceExecutedGuard>(&PP.getGuard())});

    for (unsigned Walk = 0; Walk < 24; ++Walk) {
      std::string Failure = checkOneWalk(F, Kept, Variant, Rng);
      if (Failure.empty())
        continue;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      F.print(OS);
      Graph.print(OS);
      FAIL() << "seed " << Seed << ": " << Failure << "\n" << Text;
    }
  }
}

TEST(RandomizedSoundness, RedundancyElimination) {
  checkRandomFunctions(Pipeline::Eliminate, /*WithBarriers=*/false,
                       MAIROptimizationConfig::forThreadSanitizer(), 1500);
  checkRandomFunctions(Pipeline::Eliminate, /*WithBarriers=*/false,
                       MAIROptimizationConfig::forAddressSanitizer(), 500);
}

TEST(RandomizedSoundness, RedundancyEliminationWithBarriers) {
  checkRandomFunctions(Pipeline::Eliminate, /*WithBarriers=*/true,
                       MAIROptimizationConfig::forThreadSanitizer(), 1500);
  checkRandomFunctions(Pipeline::Eliminate, /*WithBarriers=*/true,
                       MAIROptimizationConfig::forAddressSanitizer(), 500);
}

TEST(RandomizedSoundness, CommonObservationMotion) {
  checkRandomFunctions(Pipeline::CommonMotion, /*WithBarriers=*/false,
                       MAIROptimizationConfig::forThreadSanitizer(), 1000);
  checkRandomFunctions(Pipeline::CommonMotion, /*WithBarriers=*/true,
                       MAIROptimizationConfig::forThreadSanitizer(), 1000);
}

TEST(RandomizedSoundness, LoopInvariantObservationMotion) {
  checkRandomFunctions(Pipeline::LoopMotion, /*WithBarriers=*/false,
                       MAIROptimizationConfig::forThreadSanitizer(), 1000);
  checkRandomFunctions(Pipeline::LoopMotion, /*WithBarriers=*/true,
                       MAIROptimizationConfig::forThreadSanitizer(), 1000);
}

} // namespace
