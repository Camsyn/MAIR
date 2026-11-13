// Pins the semantics of the cross-block barrier queries of
// MAIRBarrierAnalysisCore: hand-written cases for cycles, irreducible cycles,
// nested loops and both directions, and a randomized comparison against a
// direct per-pair search.

#include "MAIR/MAIRBarrierAnalysis.h"
#include "MAIRPassTestUtil.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <random>

using namespace mair;

namespace {

bool isBarrierCall(const llvm::Instruction &I) {
  const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
  return Call && Call->getCalledFunction() &&
         Call->getCalledFunction()->getName() == "barrier";
}

struct BarrierCallIsBarrier {
  static bool isForwardBarrier(const llvm::Instruction &I) {
    return isBarrierCall(I);
  }
  static bool isBackwardBarrier(const llvm::Instruction &I) {
    return isBarrierCall(I);
  }
};

const llvm::BasicBlock &block(const llvm::Function &F, llvm::StringRef Name) {
  for (const llvm::BasicBlock &BB : F)
    if (BB.getName() == Name)
      return BB;
  llvm_unreachable("missing block");
}

// The definition the analysis implements, searched separately for each pair.
// Forward: a barrier block other than both ends reaches To without passing
// From, or To has a barrier and lies on a cycle that avoids From. Backward:
// the same with the edges reversed, From the covered block and To the
// witness.
bool referenceBarrierPath(const llvm::BasicBlock &From,
                          const llvm::BasicBlock &To, bool Forward) {
  const llvm::BasicBlock &Witness = Forward ? From : To;
  const llvm::BasicBlock &Covered = Forward ? To : From;
  auto HasBarrier = [](const llvm::BasicBlock *BB) {
    return llvm::any_of(*BB, isBarrierCall);
  };
  llvm::df_iterator_default_set<const llvm::BasicBlock *> Region;
  Region.insert(&Witness);
  if (Forward)
    for (const llvm::BasicBlock *BB :
         llvm::inverse_depth_first_ext(&Covered, Region))
      (void)BB;
  else
    for (const llvm::BasicBlock *BB : llvm::depth_first_ext(&Covered, Region))
      (void)BB;
  for (const llvm::BasicBlock *BB : Region)
    if (BB != &From && BB != &To && HasBarrier(BB))
      return true;
  auto InRegion = [&](const llvm::BasicBlock *BB) {
    return BB != &Witness && Region.contains(BB);
  };
  bool OnCycle = Forward ? llvm::any_of(successors(&Covered), InRegion)
                         : llvm::any_of(predecessors(&Covered), InRegion);
  return &From != &To && OnCycle && HasBarrier(&Covered);
}

TEST(MAIRBarrierAnalysis, CountsTheCoveredBlockOnlyWhenItRunsAgain) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @self(i1 %c) {
entry:
  br label %A
A:
  call void @barrier()
  br i1 %c, label %A, label %exit
exit:
  ret void
}
define void @once(i1 %c) {
entry:
  br label %A
A:
  call void @barrier()
  br label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Function &Self = *M->getFunction("self");
  const llvm::Function &Once = *M->getFunction("once");
  MAIRBarrierAnalysis<BarrierCallIsBarrier> BA;
  // A runs again before the next run of exit or after one of entry.
  EXPECT_TRUE(BA.hasFwdBarrierPath(block(Self, "entry"), block(Self, "A")));
  EXPECT_TRUE(BA.hasBwdBarrierPath(block(Self, "A"), block(Self, "exit")));
  EXPECT_FALSE(BA.hasFwdBarrierPath(block(Once, "entry"), block(Once, "A")));
  EXPECT_FALSE(BA.hasBwdBarrierPath(block(Once, "A"), block(Once, "exit")));
  // The witness block never counts, even on a cycle.
  EXPECT_FALSE(BA.hasFwdBarrierPath(block(Self, "A"), block(Self, "exit")));
  EXPECT_FALSE(BA.hasBwdBarrierPath(block(Self, "entry"), block(Self, "A")));
}

TEST(MAIRBarrierAnalysis, FollowsNestedLoopsInBothDirections) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(i1 %c1, i1 %c2) {
entry:
  br label %outer
outer:
  br label %inner
inner:
  br i1 %c1, label %body, label %latch
body:
  call void @barrier()
  br label %inner
latch:
  br i1 %c2, label %outer, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Function &F = *M->getFunction("f");
  auto B = [&](llvm::StringRef Name) -> const llvm::BasicBlock & {
    return block(F, Name);
  };
  MAIRBarrierAnalysis<BarrierCallIsBarrier> BA;
  // body reaches inner without passing outer.
  EXPECT_TRUE(BA.hasFwdBarrierPath(B("outer"), B("inner")));
  // body reaches latch only through inner.
  EXPECT_FALSE(BA.hasFwdBarrierPath(B("inner"), B("latch")));
  // The outer backedge leads around through the inner loop.
  EXPECT_TRUE(BA.hasFwdBarrierPath(B("entry"), B("outer")));
  EXPECT_TRUE(BA.hasFwdBarrierPath(B("entry"), B("exit")));
  // body is the covered block and does not run again before inner.
  EXPECT_FALSE(BA.hasBwdBarrierPath(B("body"), B("inner")));
  EXPECT_TRUE(BA.hasBwdBarrierPath(B("inner"), B("latch")));
  EXPECT_TRUE(BA.hasBwdBarrierPath(B("latch"), B("exit")));
  EXPECT_TRUE(BA.hasBwdBarrierPath(B("outer"), B("exit")));
}

TEST(MAIRBarrierAnalysis, FollowsIrreducibleCycles) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(i1 %c, i1 %d, i1 %e) {
entry:
  call void @barrier()
  br i1 %c, label %X, label %Y
X:
  br i1 %d, label %Y, label %exit
Y:
  call void @barrier()
  br i1 %e, label %X, label %exit
exit:
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Function &F = *M->getFunction("f");
  auto B = [&](llvm::StringRef Name) -> const llvm::BasicBlock & {
    return block(F, Name);
  };
  MAIRBarrierAnalysis<BarrierCallIsBarrier> BA;
  // Y reaches X through its second entry.
  EXPECT_TRUE(BA.hasFwdBarrierPath(B("entry"), B("X")));
  EXPECT_TRUE(BA.hasFwdBarrierPath(B("entry"), B("exit")));
  // Y itself is covered and runs again through X.
  EXPECT_TRUE(BA.hasFwdBarrierPath(B("entry"), B("Y")));
  EXPECT_TRUE(BA.hasBwdBarrierPath(B("X"), B("exit")));
  EXPECT_TRUE(BA.hasBwdBarrierPath(B("Y"), B("exit")));
  EXPECT_FALSE(BA.hasBwdBarrierPath(B("entry"), B("entry")));
  EXPECT_FALSE(BA.hasFwdBarrierPath(B("Y"), B("Y")));
}

TEST(MAIRBarrierAnalysis, IgnoresBarriersOutsideTheRegion) {
  llvm::LLVMContext Ctx;
  auto M = parseModule(R"IR(
declare void @barrier()
define void @f(i1 %c) {
entry:
  call void @barrier()
  br i1 %c, label %A, label %B
A:
  br label %J
B:
  br label %J
J:
  br label %after
after:
  call void @barrier()
  ret void
}
)IR",
                       Ctx);
  ASSERT_TRUE(M);
  const llvm::Function &F = *M->getFunction("f");
  auto B = [&](llvm::StringRef Name) -> const llvm::BasicBlock & {
    return block(F, Name);
  };
  MAIRBarrierAnalysis<BarrierCallIsBarrier> BA;
  EXPECT_FALSE(BA.hasFwdBarrierPath(B("entry"), B("J")));
  EXPECT_FALSE(BA.hasBwdBarrierPath(B("entry"), B("J")));
  EXPECT_FALSE(BA.hasFwdBarrierPath(B("J"), B("after")));
  EXPECT_FALSE(BA.hasBwdBarrierPath(B("J"), B("after")));
}

// Random graphs with self loops, irreducible cycles, unreachable blocks and
// blocks that cannot return, compared on every ordered pair of blocks.
llvm::Function &generateFunction(llvm::Module &M, std::mt19937 &Rng) {
  llvm::LLVMContext &Ctx = M.getContext();
  llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
  llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
  const unsigned NumBlocks = 2 + Rng() % 11;
  llvm::Function *F =
      llvm::Function::Create(llvm::FunctionType::get(VoidTy, {I32Ty}, false),
                             llvm::GlobalValue::ExternalLinkage, "f", M);
  llvm::FunctionCallee Barrier =
      M.getOrInsertFunction("barrier", llvm::FunctionType::get(VoidTy, false));
  std::vector<llvm::BasicBlock *> Blocks;
  for (unsigned I = 0; I < NumBlocks; ++I)
    Blocks.push_back(llvm::BasicBlock::Create(Ctx, "b" + llvm::Twine(I), F));
  for (llvm::BasicBlock *BB : Blocks) {
    llvm::IRBuilder<> Builder(BB);
    if (Rng() % 3 == 0)
      Builder.CreateCall(Barrier);
    // The entry block cannot be a successor.
    auto Pick = [&] { return Blocks[1 + Rng() % (NumBlocks - 1)]; };
    unsigned NumSuccs = Rng() % 4;
    if (NumSuccs == 0) {
      Builder.CreateRetVoid();
      continue;
    }
    llvm::SwitchInst *Switch =
        Builder.CreateSwitch(F->getArg(0), Pick(), NumSuccs - 1);
    for (unsigned I = 1; I < NumSuccs; ++I)
      Switch->addCase(Builder.getInt32(I), Pick());
  }
  return *F;
}

TEST(MAIRBarrierAnalysis, MatchesPerPairSearchOnRandomGraphs) {
  std::mt19937 Rng(20261113);
  for (unsigned Round = 0; Round < 300; ++Round) {
    llvm::LLVMContext Ctx;
    llvm::Module M("random", Ctx);
    llvm::Function &F = generateFunction(M, Rng);
    ASSERT_FALSE(llvm::verifyFunction(F, &llvm::errs()));
    MAIRBarrierAnalysis<BarrierCallIsBarrier> BA;
    for (const llvm::BasicBlock &From : F)
      for (const llvm::BasicBlock &To : F) {
        SCOPED_TRACE(("round " + llvm::Twine(Round) + ": " + From.getName() +
                      " -> " + To.getName())
                         .str());
        EXPECT_EQ(BA.hasFwdBarrierPath(From, To),
                  referenceBarrierPath(From, To, /*Forward=*/true));
        EXPECT_EQ(BA.hasBwdBarrierPath(From, To),
                  referenceBarrierPath(From, To, /*Forward=*/false));
      }
  }
}

} // namespace
