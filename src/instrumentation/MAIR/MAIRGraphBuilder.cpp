#include "MAIR/MAIR.h"

#include "MAIR/MAIRInternal.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CycleInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

namespace {
const Instruction *getBarrierRangeStart(const ProgramPoint &PP) {
  return PP.isBasicBlockEntrySite() ? nullptr : &PP.getAnchor();
}

const Instruction *getBarrierRangeEnd(const ProgramPoint &PP) {
  return &PP.getAnchor();
}

bool isInterestingMop(const Instruction &I) {
  if (I.hasMetadata(LLVMContext::MD_nosanitize))
    return false;
  return isa<LoadInst, StoreInst, AtomicRMWInst, AtomicCmpXchgInst,
             AnyMemIntrinsic>(I) ||
         isMaskedMemoryIntrinsic(I);
}

void verifyNaturalLoops(const LoopInfo &LI) {
  for (const Loop *Loop : LI.getLoopsInPreorder()) {
    if (!Loop->getLoopPreheader())
      report_fatal_error(
          "MAIR requires a loop preheader before graph building");
    if (!Loop->hasDedicatedExits())
      report_fatal_error(
          "MAIR requires dedicated loop exits before graph building");
  }
}

// Blocks of irreducible cycles. LoopInfo does not represent these cycles, so
// they have no MAIR loop scope of their own.
//
// TODO: Non-standard loops are not optimized yet. Observations in the cycle
// stay where they are; represent and optimize the region later.
SmallPtrSet<const BasicBlock *, 16> collectUnsupportedLoopBlocks(Function &F) {
  CycleInfo Cycles;
  Cycles.compute(F);
  SmallPtrSet<const BasicBlock *, 16> Unsupported;
  SmallVector<const Cycle *, 8> Worklist(Cycles.toplevel_cycles());
  while (!Worklist.empty()) {
    const Cycle *C = Worklist.pop_back_val();
    if (C->isReducible())
      append_range(Worklist, C->children());
    else
      // The blocks of a cycle include those of its sub-cycles.
      Unsupported.insert_range(C->blocks());
  }
  return Unsupported;
}

// Whether every path from \p From reaches \p To, which post-dominates it and
// is in the same loop \p L, before starting another iteration of L or leaving
// it. A value defined in L is the same value at both blocks only then.
bool postDominatesWithinIteration(const BasicBlock &From, const BasicBlock &To,
                                  const Loop *L) {
  if (!L)
    return true;
  const BasicBlock *Header = L->getHeader();
  if (&To == Header)
    return false;
  SmallPtrSet<const BasicBlock *, 16> Visited;
  SmallVector<const BasicBlock *, 16> Worklist{&From};
  while (!Worklist.empty())
    for (const BasicBlock *Succ : successors(Worklist.pop_back_val())) {
      if (Succ == &To)
        continue;
      // A path that leaves L can come back to To only through the header.
      if (Succ == Header || !L->contains(Succ))
        return false;
      if (Visited.insert(Succ).second)
        Worklist.push_back(Succ);
    }
  return true;
}

} // namespace

// Build the initial, unoptimized MAIR graph for one LLVM function.
//
// ProgramPoint layout follows each LLVM basic block:
//   BBEntry -> memory-instruction program points in source order -> BBExit.
// Edges are then added for adjacent program points inside each block and for
// immediate dominator/post-dominator relationships across blocks. Barrier
// queries decide whether the corresponding forward/backward sanitizer
// reachability edge is legal.
MAIRGraph MAIRGraph::build(Function &F, const DominatorTree &DT,
                           const PostDominatorTree &PDT, const LoopInfo &LI,
                           MAIRBarrierAnalysisCore &BA,
                           MAIROptimizationConfig Config) {
  verifyNaturalLoops(LI);
  SmallPtrSet<const BasicBlock *, 16> Unsupported =
      collectUnsupportedLoopBlocks(F);
  MAIRGraph Graph(F, Config);
  for (const BasicBlock *BB : Unsupported)
    Graph.addUnsupportedLoopBlock(*BB);
  Graph.Analyses.bind(DT, PDT, LI);
  DenseMap<const Loop *, MAIRGraph *> LoopGraphs;
  for (const Loop *Loop : LI.getLoopsInPreorder()) {
    MAIRGraph *Parent = Loop->getParentLoop()
                            ? LoopGraphs.lookup(Loop->getParentLoop())
                            : &Graph;
    auto Body = std::unique_ptr<MAIRGraph>(new MAIRGraph(*Parent, *Loop));
    LoopGraphs[Loop] = Body.get();
    Parent->ChildGraphs.push_back(std::move(Body));
  }
  auto ScopeFor = [&](const BasicBlock &BB) -> MAIRGraph & {
    const Loop *Loop = LI.getLoopFor(&BB);
    return Loop ? *LoopGraphs.lookup(Loop) : Graph;
  };
  using BlockPoints =
      DenseMap<const BasicBlock *, SmallVector<ProgramPoint *, 4>>;
  DenseMap<MAIRGraph *, BlockPoints> OrderedPPs;

  // ===========================================================
  // 1. Create program points for memory instructions and block boundaries
  // ===========================================================
  for (BasicBlock &BB : F) {
    MAIRGraph &Scope = ScopeFor(BB);
    Scope.ForwardBarrier |= BA.hasFwdBarrier(BB);
    Scope.BackwardBarrier |= BA.hasBwdBarrier(BB);
    auto &PPsInOneBB = OrderedPPs[&Scope][&BB];
    auto &Start = Scope.createBBProgramPoint(BB, true);
    PPsInOneBB.push_back(&Start);
    for (Instruction &I : BB) {
      if (isInterestingMop(I)) {
        auto &PP = Scope.createInstProgramPoint(BB, I);
        PPsInOneBB.push_back(&PP);
      }
    }
    auto &End = Scope.createBBProgramPoint(BB, false);
    PPsInOneBB.push_back(&End);
  }
  for (const Loop *Loop : LI.getLoopsInPreorder()) {
    MAIRGraph &Body = *LoopGraphs.lookup(Loop);
    MAIRGraph &Parent = *Body.Parent;
    BasicBlock *Preheader = Loop->getLoopPreheader();
    Body.LoopPreheader = &Parent.createLoopProgramPoint(
        ProgramPointSite::anchored(*Preheader->getTerminator(),
                                   ProgramPointSite::Kind::BasicBlockExit),
        LoopProgramPoint::BoundaryKind::LoopPreheader, Body);
    OrderedPPs[&Parent][Preheader].push_back(Body.LoopPreheader);
    SmallVector<BasicBlock *, 4> ExitBlocks;
    Loop->getUniqueExitBlocks(ExitBlocks);
    for (BasicBlock *Exit : ExitBlocks) {
      auto &Boundary = Parent.createLoopProgramPoint(
          ProgramPointSite::anchored(*Exit->getFirstNonPHIIt(),
                                     ProgramPointSite::Kind::BasicBlockEntry),
          LoopProgramPoint::BoundaryKind::LoopExit, Body);
      Body.LoopExits.push_back(&Boundary);
      auto &Points = OrderedPPs[&Parent][Exit];
      Points.insert(Points.begin(), &Boundary);
    }
  }

  // Propagate the barrier status from children to parents.
  Graph.propagateBarrier();

  // ===========================================================
  // 2. Connect program point edges inside each block
  // ===========================================================
  for (auto &ScopeEntry : OrderedPPs) {
    MAIRGraph &Scope = *ScopeEntry.first;
    for (auto &Entry : ScopeEntry.second) {
      const BasicBlock &BB = *Entry.first;
      ArrayRef<ProgramPoint *> Points = Entry.second;
      for (unsigned I = 1; I < Points.size(); ++I) {
        ProgramPoint &Prev = *Points[I - 1], &Next = *Points[I];
        const Instruction *PrevI = getBarrierRangeStart(Prev);
        const Instruction *NextI = getBarrierRangeEnd(Next);
        bool FwdEndpointBarrier =
            Prev.isMemoryInstSite() && PrevI && BA.isFwdBarrier(*PrevI);
        bool BwdEndpointBarrier =
            Prev.isMemoryInstSite() && PrevI && BA.isBwdBarrier(*PrevI);
        const bool SameSite = Prev.getSite() == Next.getSite();
        if (SameSite ||
            (!FwdEndpointBarrier && !BA.hasFwdBarrierBetween(BB, PrevI, NextI)))
          Scope.addEdge(Prev, Next, PPEdgeKind::Dom);
        if (SameSite ||
            (!BwdEndpointBarrier && !BA.hasBwdBarrierBetween(BB, PrevI, NextI)))
          Scope.addEdge(Next, Prev, PPEdgeKind::PostDom);
      }
    }
  }

  // ===========================================================
  // 3. Connect program point edges across blocks via DT/PDT
  // ===========================================================
  auto GetIDom = [](auto &DT, const BasicBlock &BB) -> const BasicBlock * {
    if (const DomTreeNode *Node = DT.getNode(&BB))
      if (const DomTreeNode *IDom = Node->getIDom())
        return IDom->getBlock();
    return nullptr;
  };
  for (BasicBlock &BB : F) {
    MAIRGraph &Scope = ScopeFor(BB);
    ProgramPoint *EntryPP = OrderedPPs[&Scope][&BB].front();
    ProgramPoint *ExitPP = OrderedPPs[&Scope][&BB].back();
    assert(EntryPP && ExitPP);
    const BasicBlock *IDom = GetIDom(DT, BB);
    while (IDom && &ScopeFor(*IDom) != &Scope)
      IDom = GetIDom(DT, *IDom);
    if (IDom) {
      ProgramPoint *Pred = OrderedPPs[&Scope][IDom].back();
      if (!BA.isFwdBarrier(*IDom->getTerminator()) &&
          !BA.hasFwdBarrierPath(*IDom, BB))
        Scope.addEdge(*Pred, *EntryPP, PPEdgeKind::Dom);
    }
    const BasicBlock *IPDom = GetIDom(PDT, BB);
    while (IPDom && &ScopeFor(*IPDom) != &Scope)
      IPDom = GetIDom(PDT, *IPDom);
    if (IPDom) {
      ProgramPoint *Succ = OrderedPPs[&Scope][IPDom].front();
      if (!BA.isBwdBarrier(*BB.getTerminator()) &&
          postDominatesWithinIteration(BB, *IPDom, Scope.getLoop()) &&
          !BA.hasBwdBarrierPath(BB, *IPDom))
        Scope.addEdge(*Succ, *ExitPP, PPEdgeKind::PostDom);
    }
  }

  return Graph;
}

} // namespace mair
