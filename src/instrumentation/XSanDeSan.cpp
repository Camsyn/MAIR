//===----------------------------------------------------------------------===//
// DeSan workflow
//===----------------------------------------------------------------------===//
//
// XSan runs each normal sanitizer pass on top of the current user IR, then
// immediately runs DeSan for that sanitizer. DeSan restores the module back to
// the user IR shape required by the next sanitizer and archives the removed
// sanitizer semantics for ReSan.
//
// 1. Sanitizer pass: run one sanitizer on the current user IR.
//    1.1. Livepatch sanitizers: record add/delete/replace/function edits.
//    1.2. UBSan: rely on frontend UBSanInst tags because clang emits checks
//         before XSan can record livepatch events.
//
// 2. Generic DeSan restore: for ASan/TSan/MSan, use UserIR marks as the
//    boundary and restore livepatch events in dependency order.
//    2.1. restoreFunction: detach sanitizer-only functions.
//    2.2. restoreDelete: restore deleted user instructions.
//    2.3. restoreReplace: restore replaced user values.
//    2.4. restoreAdd: extract sanitizer additions after user IR is restored.
//
// 3. restoreAdd / SanInsts: scan each function while CFG is still intact.
//    3.1. Pure add: detach sanitizer instructions from a user BB.
//    3.2. Split add: move user suffix instructions back from sink blocks.
//    3.3. Redirect add: fold sanitizer-only edges to the user sink and record
//         the edges needed by ReSan.
//
// 4. restoreAdd / SanPHIs: scan each function again after SanInst extraction.
//    4.1. Find sanitizer PHIs that still live in user BB PHI lists.
//    4.2. Detach those PHIs into PHIAddRecords before SanBBs are removed.
//
// 5. restoreAdd / SanBBs: detach sanitizer-only basic blocks last.
//    5.1. Preserve block insertion order in BlockAddRecords.
//    5.2. Break sanitizer CFG edges only after SanInsts and SanPHIs are
//         archived.
//
// 6. UBSan frontend recovery: run before UBSan restoreAdd.
//    6.1. Fold frontend overflow intrinsics back into user arithmetic.
//    6.2. Record paired replace/delete facts for ReSan.
//    6.3. Collect UBSan user BBs, then reuse the same AddArchive extraction.
//    6.4. Keep UBSan split classification because optimized frontend IR can
//         obscure the original user CFG shape.
//
//===----------------------------------------------------------------------===//

#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"
#include "Utils/UbsanUtils.h"
#include "XSanDeSanReSan.h"
#include "XSanSanitizingContext.h"
#include "livepatch/record/SanitizingRecorder.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLForwardCompat.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/Utils/Local.h"

#include <cassert>
#include <cstdint>
#include <optional>

using namespace llvm;

namespace __xsan::desan {
namespace {
bool isSanInst(const Sanitizer &San, const Instruction &I) {
  return San.isUBSan() ? UBSanInst::is(I) : !__xsan::UserIR::is(I);
}
bool isUsrInst(const Sanitizer &San, const Instruction &I) {
  return !isSanInst(San, I);
}

bool replaceNonSanInstUses(const Sanitizer &San, Value &From, Value &To) {
  if (From.use_empty())
    return false;
  bool Changed = false;
  From.replaceUsesWithIf(&To, [&](Use &U) -> bool {
    auto *I = dyn_cast<Instruction>(U.getUser());
    const bool IsSanUse = I && isSanInst(San, *I);
    Changed |= !IsSanUse;
    return !IsSanUse;
  });
  return Changed;
}

BasicBlock::iterator nextSanInst(BasicBlock::iterator SI,
                                 BasicBlock::iterator EI,
                                 const Sanitizer &San) {
  return llvm::find_if(
      llvm::make_range(SI, EI),
      [&San](const Instruction &I) -> bool { return isSanInst(San, I); });
}

BasicBlock::iterator nextUsrInst(BasicBlock::iterator SI,
                                 BasicBlock::iterator EI,
                                 const Sanitizer &San) {
  return llvm::find_if(
      llvm::make_range(SI, EI),
      [&San](const Instruction &I) -> bool { return isUsrInst(San, I); });
}

Instruction *nextUsrInst(BasicBlock &BB, const Sanitizer &San) {
  auto End = BB.end();
  auto It = nextUsrInst(BB.begin(), End, San);
  return It == End ? nullptr : &*It;
}

bool matchSpecialBranchValue(const BasicBlock &SrcBB, const Value *&CondVal,
                             const Constant *&ImposedBySan) {
  using namespace llvm::PatternMatch;
  CondVal = ImposedBySan = nullptr;
  Instruction *Cond;
  if (!match(SrcBB.getTerminator(),
             m_Br(m_Instruction(Cond), m_BasicBlock(), m_BasicBlock())))
    return false;

  // Match equality branch:
  //   br i1 (icmp eq/ne V, C), label %Then, label %Else
  //   Sink:
  //     %p = phi [ V, %Src ], [ C, %SanBB ]
  // Treat C from non-SrcBB incoming edges as V.
  CmpPredicate CmpPred;

  const Value *LHS = nullptr, *RHS = nullptr;
  Constant *C = nullptr;
  if (match(Cond, m_ICmp(CmpPred, m_Value(LHS), m_Constant(C))) &&
      CmpInst::isEquality(CmpPred) && !isa<Constant>(LHS)) {
    // TODO: support a non-constant RHS once the needed equivalence boundary is
    // explicit.
    CondVal = LHS, ImposedBySan = C;
    return true;
  }

  // Match signed add/sub overflow branch, e.g.:
  //   %ov = call { iN, i1 } @llvm.sadd.with.overflow.iN(iN %V, iN 1)
  //   %ob = extractvalue { iN, i1 } %ov, 1
  //   br i1 %ob, label %SanBB, label %Cont
  //   Sink:
  //     %p = phi [ V, %Src ], [ iN::smax() - 1, %SanBB ]
  // Also match the inverted condition:
  //   br i1 (xor %ob, true), label %Cont, label %SanBB
  // For ssub.with.overflow(V, 1), the matched boundary is signed_min + 1.
  WithOverflowInst *WOI = nullptr;
  if (!match(Cond, m_ExtractValue<1>(m_WithOverflowInst(WOI))))
    return false;
  bool IsSigned, IsAdd;
  switch (WOI->getIntrinsicID()) {
  case Intrinsic::sadd_with_overflow:
    IsSigned = true, IsAdd = true;
    break;
  case Intrinsic::ssub_with_overflow:
    IsSigned = true, IsAdd = false;
    break;
  case Intrinsic::uadd_with_overflow:
    IsSigned = false, IsAdd = true;
    break;
  case Intrinsic::usub_with_overflow:
    IsSigned = false, IsAdd = false;
    break;
  default:
    return false;
  }
  LHS = WOI->getArgOperand(0), RHS = WOI->getArgOperand(1);
  auto *Ty = dyn_cast<IntegerType>(LHS->getType());
  if (!Ty)
    return false;

  auto isOne = [](const Value *V) {
    const auto *C = dyn_cast<ConstantInt>(V);
    return C && C->isOne();
  };
  const bool IsRHSOne = isOne(RHS);
  if (IsAdd && (IsRHSOne || isOne(LHS)) || !IsAdd && IsRHSOne) {
    const APInt Boundary =
        IsAdd ? (IsSigned ? APInt::getSignedMaxValue(Ty->getBitWidth())
                          : APInt::getMaxValue(Ty->getBitWidth()))
              : (IsSigned ? APInt::getSignedMinValue(Ty->getBitWidth())
                          : APInt::getMinValue(Ty->getBitWidth()));
    CondVal = IsRHSOne ? LHS : RHS,
    ImposedBySan = ConstantInt::get(Ty, Boundary);
    return true;
  }

  return false;
}

struct SplitAddExtraction {
  BasicBlock &SinkBB;
  Instruction *SinkUserI = nullptr;
  bool ReuseExistingSink = false;
  AddRecord::SanBBsVec SanBBs;
  mutable SmallSetVector<BasicBlock *, 2> SinkSanPreds;
  mutable const Value *CondVal = nullptr;
  mutable const Constant *ImposedBySan = nullptr;
};

// Return if all IVs of preds ∈ (preds(BB) ∩ Visisted), and pass (preds(BB) ∩
// Visisted) as mutable argument.
bool AllSpecifiedPredIVsEquals(const BasicBlock &SrcBB,
                               const SmallPtrSetImpl<BasicBlock *> &Visited,
                               const SplitAddExtraction &Redirect) {
  assert(Redirect.ReuseExistingSink);
  const BasicBlock &BB = Redirect.SinkBB;
  auto &SpecifiedPreds = Redirect.SinkSanPreds;
  // This sink is the only user exit and we have finished the DFS.
  SpecifiedPreds.clear();
  for (const auto *Pred : predecessors(&BB))
    if (Visited.contains(Pred))
      SpecifiedPreds.insert(const_cast<BasicBlock *>(Pred));

  auto &CondVal = Redirect.CondVal;
  auto &ImposedBySan = Redirect.ImposedBySan;
  const bool Special = matchSpecialBranchValue(SrcBB, CondVal, ImposedBySan);
  auto AllIVsEq = [&](const PHINode &PHi) {
    const unsigned NumIV = PHi.getNumIncomingValues();
    const Value *IV = nullptr;
    for (unsigned Idx = 0; Idx != NumIV; ++Idx) {
      BasicBlock *Pred = PHi.getIncomingBlock(Idx);
      if (!SpecifiedPreds.contains(Pred))
        continue;
      const Value *CurIV = PHi.getIncomingValue(Idx);
      if (Special && Pred != &SrcBB && CurIV == ImposedBySan)
        CurIV = CondVal;
      if (!IV)
        IV = CurIV;
      else if (IV != CurIV)
        return false;
    }
    return true;
  };
  return all_of(BB.phis(), AllIVsEq);
}
} // namespace

PreprocessPass::PreprocessPass(std::shared_ptr<SanitizingContext> Ctx)
    : Ctx(std::move(Ctx)) {}

PreservedAnalyses PreprocessPass::run(Module &M, ModuleAnalysisManager &MAM) {
  requireOrDie(!!Ctx, "desan preprocess pass missing pipeline context");

  Ctx->resetDumpSequence();
  Ctx->dumpNextIR(M, "raw");

  resetSession();

  // Do dead code elimination first to avoid sanitizer remove user basic blocks,
  // which we can't deal with for now.
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (Function &F : M) {
    if (!F.isDeclaration() && llvm::removeUnreachableBlocks(F)) {
      while (llvm::removeUnreachableBlocks(F))
        ;
      FAM.invalidate(F, PreservedAnalyses::none());
    }
  }

  Ctx->clearArchives();

  return PreservedAnalyses::all();
}

UserTaggingPass::UserTaggingPass(std::shared_ptr<SanitizingContext> Ctx)
    : Ctx(std::move(Ctx)) {}

PreservedAnalyses UserTaggingPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!Ctx, "desan user tagging pass missing pipeline context");
  requireOrDie(Ctx->UBSanDeSanCompleted,
               "user tagging must run after UBSan DeSan");

  markUserIR(M);
  Ctx->UserBBs = collectUserBBs(M);

  return PreservedAnalyses::all();
}

DeSanPass::DeSanPass(Sanitizer San, std::shared_ptr<SanitizingContext> Ctx)
    : San(San), Ctx(std::move(Ctx)) {}

PreservedAnalyses DeSanPass::run(Module &M, ModuleAnalysisManager &MAM) {
  requireOrDie(!!Ctx, "desan restore pass missing pipeline context");

  if (San.isUBSan() && !__xsan::hasUbsanFunction(M)) {
    Ctx->UBSanDeSanCompleted = true;
    return PreservedAnalyses::all();
  }

  Ctx->dumpNextIR(M, San.isUBSan() ? "ubsan-tagging" : San.name());

  SanitizerArchive &Archive = Ctx->addArchive(San);
  runDeSan(M, Archive, Ctx->UserBBs);
  if (San.isUBSan())
    Ctx->UBSanDeSanCompleted = true;

  Ctx->dumpNextIR(M, "desan");

  return PreservedAnalyses::none();
}

static void restoreFunction(const Sanitizer &San, FunctionArchive &Archive,
                            Module &M, const UserBBsMap &UserBBsMap) {
  size_t BeginSize = Archive.size();

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (UserBBsMap.count(&F))
      continue;
    Archive.push_back(&F);
  }

  for (size_t I = BeginSize; I < Archive.size(); ++I) {
    Archive[I]->removeFromParent();
  }
}

bool DeleteArchive::restoreEvent(DeleteEvent &Event) {
  // The prerequisite anchor has not been restore.
  if (Event.Anchor && !Event.Anchor->getParent())
    return false;

  // Restore the instruction before anchor
  if (!Event.Anchor)
    __xsan::compat::appendInstruction(Event.Block, Event.Inst2Remove);
  else
    __xsan::compat::insertInstructionBefore(Event.Inst2Remove, Event.Anchor);
  // Restore the operands
  for (auto [Op, OpUse] :
       llvm::zip(Event.Operands, Event.Inst2Remove->operands()))
    OpUse.set(Op);
  // Add to archive
  this->insert(Event.Inst2Remove);
  return true;
}

static void restoreDelete(const Sanitizer &San, DeleteArchive &Archive,
                          const std::vector<DeleteEvent> &Events) {
  llvm::SmallVector<DeleteEvent *, 8> Worklist, Pending;
  Worklist.reserve(Events.size());
  for (const auto &Event : Events)
    Worklist.push_back(const_cast<DeleteEvent *>(&Event));

  // Some restore of DeleteEvent might depend on other DeleteEvent's restore.
  // Hence, we use Worklist to reach the fixpoint.
  bool Changed;
  do {
    Changed = false;
    for (auto &Event : make_pointee_range(Worklist)) {
      if (Archive.restoreEvent(Event))
        Changed = true;
      else
        Pending.push_back(&Event);
    }
    Worklist.swap(Pending);
    Pending.clear();
  } while (Changed);
  requireOrDie(Worklist.empty(), "Cannot restore some Inst2Remove");
}

static void restoreReplace(const Sanitizer &San, ReplaceArchive &Archive,
                           const std::vector<ReplaceEvent> &Events) {
  for (const auto &Event : Events) {
    const bool RealReplacement =
        replaceNonSanInstUses(San, *Event.SanReplacer, *Event.UserReplacee);
    Archive[Event.UserReplacee].push_back({Event.SanReplacer, RealReplacement});
  }
}
namespace {
/// All insts are UBSan and term with ret/unreachable
bool hasNoUserExit(const BasicBlock *BB) {
  const bool AllUBSan =
      BB && all_of(*BB, [](const Instruction &I) { return UBSanInst::is(I); });
  return AllUBSan && (isa<UnreachableInst>(BB->getTerminator()) ||
                      isa<ReturnInst>(BB->getTerminator()));
}
class UBSanBBLattice {
public:
  bool isAsUser() const { return K == User; }
  bool isAsSan() const { return K == San; }
  bool markAsUser(const BasicBlock *Cur) {
    if (isAsUser())
      return false;
    UserEntry = Cur;
    K = User;
    Fixed = true;
    return true;
  }
  template <class LatticeRange>
  bool merge(const BasicBlock *Cur, LatticeRange &&PredStates) {
    if (Fixed)
      return false;
    const BasicBlock *UsrEntry = nullptr;
    for (const UBSanBBLattice &L : PredStates) {
      if (!L.UserEntry) {
        assert(L.K == Unknown && "Unexpected Pred Lattice");
        continue;
      }
      if (!UsrEntry) {
        UsrEntry = L.UserEntry;
        continue;
      }
      // Has >1 user entries
      if (UsrEntry != L.UserEntry)
        return hasNoUserExit(Cur) ? markAsNoUsrExitSan() : markAsUser(Cur);
    }
    if (!UsrEntry)
      return false;
    return markAsSan(UsrEntry);
  }

private:
  bool markAsNoUsrExitSan() {
    if (isAsUser())
      return false;
    K = San;
    UserEntry = nullptr;
    Fixed = true;
    return true;
  }
  bool markAsSan(const BasicBlock *Entry) {
    if (isAsUser() || isAsSan() && UserEntry == Entry)
      return false;
    UserEntry = Entry;
    K = San;
    return true;
  }

  // Unknown ──> San -> User
  enum Kind { Unknown, San, User } K = Unknown;
  bool Fixed = false;
  const BasicBlock *UserEntry = nullptr;
};

bool anyPredTermNonUBSan(const BasicBlock *BB) {
  assert(BB->getTerminator() && "Pathological BB without Terminator");
  return any_of(predecessors(BB), [](const BasicBlock *Pred) {
    return !UBSanInst::is(*Pred->getTerminator());
  });
}
} // namespace

/// For UBSan, the defitions of UserBB and SanBB are different from otehr San.
/// UserBB defitions:
///   1. Function entry
///   2. BB with any predecessor terminated by non-UBSan terminator
///   3. BB reaching >1 user entries, computed by a fixpoint lattice:
///      Unknown -> San(UserEntry) -> User.
/// SanBB defitions: non-UserBB.
/// SanBB's nature:
///   1. Either has only ONE user entry
///   2. Or has no user exit
///
/// We use a fixpoint iteration to infer a user bb set.
static UserBBsMap collectUserBBsForUBSan(Module &M) {
  UserBBsMap Result;
  DenseMap<const BasicBlock *, UBSanBBLattice> States;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    States.clear();
    SmallSetVector<const BasicBlock *, 32> WorkList;
    SmallPtrSet<const BasicBlock *, 32> Processed;

    // RULE 1: Function entry is user.
    States[&F.getEntryBlock()].markAsUser(&F.getEntryBlock());
    auto Update = [&](const BasicBlock *BB) {
      return States[BB].merge(
          BB, map_range(predecessors(BB), [&States](const BasicBlock *Pred) {
            return States[Pred];
          }));
    };

    // 1. First round use Reverse Post Order Traversal to speed up the analysis.
    ReversePostOrderTraversal RPO(&F);
    for (auto *BB : RPO) {
      Processed.insert(BB);
      // RULE 2: BB is user if any predecessor terminated by user termniator.
      if (anyPredTermNonUBSan(BB))
        States[BB].markAsUser(BB); // Fixed as User
      // RULE 3: BB is user if it has >1 user entries and a user exit
      else
        Update(BB);
      // If any of successors are processed before, put them into the worklist.
      WorkList.insert_range(
          make_filter_range(successors(BB), [&](const BasicBlock *Succ) {
            return Processed.contains(Succ);
          }));
    }

    // 2. Worklist algorithm to perform fixpoint iteration.
    while (!WorkList.empty()) {
      const BasicBlock *BB = WorkList.pop_back_val();
      if (!Update(BB))
        continue;
      for (const BasicBlock *Succ : successors(BB))
        WorkList.insert(Succ);
    }

    // 3. Collect user BBs
    auto &UserBBsSet = Result[&F];
    for (auto &[BB, State] : States)
      if (State.isAsUser())
        UserBBsSet.insert(const_cast<BasicBlock *>(BB));
  }
  return Result;
}

AddRecord AddRecord::extractFrom(BasicBlock &SrcBB, BasicBlock::iterator Fr,
                                 BasicBlock::iterator To) {
  requireOrDie(Fr != To, "desan pure add resume got empty instruction range");
  auto SanInsts = std::unique_ptr<BasicBlock>(
      BasicBlock::Create(SrcBB.getContext(), MagicRecordBbName));
  __xsan::compat::spliceInstructions(SanInsts.get(), SanInsts->end(), &SrcBB,
                                     Fr, To);
  return {AddRecordKind::Pure, &*To, std::move(SanInsts), {}, PureExtra()};
}

AddRecord AddRecord::extractFrom(llvm::BasicBlock &SrcBB,
                                 llvm::BasicBlock::iterator Fr,
                                 llvm::BasicBlock &SinkBB,
                                 llvm::BasicBlock::iterator To,
                                 SanBBsVec &&SanBBs) {
  requireOrDie(Fr != SrcBB.end(),
               "desan split add resume got empty source suffix");
  requireOrDie(To != SinkBB.end(),
               "desan split add resume got sink without user suffix");
  SplitExtra Split{&SrcBB, &SinkBB};

  // Expected: Src -> Sink would be merged as Src, hence we need to update phis
  // of successors.
  SinkBB.replaceSuccessorsPhiUsesWith(&SinkBB, &SrcBB);

  auto SanInsts = std::unique_ptr<BasicBlock>(
      BasicBlock::Create(SrcBB.getContext(), MagicRecordBbName));

  // Merge SrcBB -> ... -> SinkBB as SrcBB

  // SanInsts = SrcBB[Fr:]
  // SrcBB'   = SrcBB[:Fr]
  __xsan::compat::spliceInstructions(SanInsts.get(), SanInsts->end(), &SrcBB,
                                     Fr, SrcBB.end());
  // SrcBB'  = SrcBB[:Fr] : SinkBB[To:]
  // SinkBB' = SinkBB[:To]
  __xsan::compat::spliceInstructions(&SrcBB, SrcBB.end(), &SinkBB, To,
                                     SinkBB.end());

  requireOrDie(SanInsts->getTerminator() != nullptr,
               "desan split add lost archived terminator");
  return {AddRecordKind::Split, &*To, std::move(SanInsts), std::move(SanBBs),
          std::move(Split)};
}

AddRecord AddRecord::extractFrom(
    llvm::BasicBlock &SrcBB, llvm::BasicBlock::iterator Fr,
    llvm::BasicBlock &SinkBB, const llvm::Value *CondVal,
    const llvm::Constant *ImposedBySan,
    llvm::SmallSetVector<llvm::BasicBlock *, 2> &&DetachedEdges,
    SanBBsVec &&SanBBs) {
  requireOrDie(Fr != SrcBB.end(),
               "desan split redirect got empty source suffix");

  auto SanInsts = std::unique_ptr<BasicBlock>(
      BasicBlock::Create(SrcBB.getContext(), MagicRecordBbName));
  __xsan::compat::spliceInstructions(SanInsts.get(), SanInsts->end(), &SrcBB,
                                     Fr, SrcBB.end());
  auto *NewBr = BranchInst::Create(&SinkBB, &SrcBB);
  __xsan::UserIR::set(*NewBr);
  // User SrcBB is unstable for replay, remove it.
  DetachedEdges.remove(&SrcBB);
  RedirectExtra Redirect{&SrcBB, &SinkBB, std::move(DetachedEdges), CondVal,
                         ImposedBySan};

  // Fold Phi IVs in Sink BB.
  Redirect.removeEdgesOfSanBB2Sink(SrcBB, *SanInsts);
  requireOrDie(SanInsts->getTerminator() != nullptr,
               "desan split add lost archived terminator");
  return {AddRecordKind::Redirect, NewBr, std::move(SanInsts),
          std::move(SanBBs), std::move(Redirect)};
}

void BlockAddRecord::revert() {
  for (auto [SanBB, UserSucc] : SanBBs) {
    assert(SanBB && SanBB->getParent());
    // Some SinkBB has no terminator as SplitAdd.
    auto *Term = SanBB->getTerminator();
    // Hide the edge SanBB -> UserSucc to avoid predecessors(UserSucc) noticing
    // SanBB.
    if (UserSucc && Term)
      Term->replaceSuccessorWith(UserSucc, nullptr);
    SanBB->removeFromParent();
  }
}

std::optional<PHIAddRecord> PHIAddRecord::extractFrom(const Sanitizer &San,
                                                      BasicBlock &TargetUsrBB) {
  SmallVector<PHINode *, 2> SanPhis(make_pointer_range(
      make_filter_range(TargetUsrBB.phis(), [&San](const PHINode &PHI) {
        return isSanInst(San, PHI);
      })));
  if (SanPhis.empty())
    return std::nullopt;
  // detach
  for (PHINode *Phi : SanPhis)
    Phi->removeFromParent();
  return PHIAddRecord{std::move(SanPhis), &TargetUsrBB};
}

namespace {

std::optional<SplitAddExtraction> extractSplitAdd(const Sanitizer &San,
                                                  BasicBlock &SourceBB,
                                                  const BBsSet &Users) {
  df_iterator_default_set<BasicBlock *, 8> Visited;
  auto It = /* Skip Root*/ df_ext_begin(&SourceBB, Visited),
       End = df_ext_end(&SourceBB, Visited);
  ++It;
  // Find the next user exit of current source BB.
  // Use DFS to search.
  auto NextSink = [&]() -> std::optional<SplitAddExtraction> {
    while (It != End) {
      BasicBlock &Cur = **It;
      if (Users.contains(&Cur)) {
        It.skipChildren();
        // If the sink BB is a user BB, we need to retain it from Functon.
        return SplitAddExtraction{Cur, nullptr,
                                  /*ReuseExistingSink=*/true};
      }
      if (Instruction *UserI = nextUsrInst(Cur, San)) {
        It.skipChildren();
        return SplitAddExtraction{Cur, UserI,
                                  /*ReuseExistingSink=*/false};
      }
      ++It;
    }
    return std::nullopt;
  };
  // For Non-UBSan sanitizer, we assume that there is only one user exit.
  auto Sink = NextSink();
  // After optimization, some UBSan San entry might has no exit.
  if (!Sink.has_value() && San.isUBSan())
    return std::nullopt;

  requireOrDie(Sink.has_value(), "[DeSan] No sink found in fun: " +
                                     SourceBB.getParent()->getName() +
                                     "\n\tSourceBB: " + SourceBB.getName() +
                                     "\n\tSan     : " + San.name() + "\n");
  if (NextSink()) {
    // UBSan sanitizing might be affected by optimization, causing >1 user
    // exits. For this case, we bail out conservatively.
    requireOrDie(San.isUBSan(), "[DeSan] Found >1 sinks for a SourceBB: " +
                                    SourceBB.getName() +
                                    "\n\tSan: " + San.name() + "\n");
    return std::nullopt;
  }
  if (Sink->ReuseExistingSink) {
    // This sink is the only user exit and we have finished the DFS.
    // Any of IVs not equal, cannot merge.
    if (!AllSpecifiedPredIVsEquals(SourceBB, Visited, *Sink))
      return std::nullopt;
    Visited.erase(&Sink->SinkBB);
  }
  Visited.erase(&SourceBB);
  // The retained Visisted BBs are all SanBBs.
  Sink->SanBBs.append(Visited.begin(), Visited.end());
  return *Sink;
}
} // namespace

void AddRecord::RedirectExtra::removeEdgesOfSanBB2Sink(BasicBlock &SourceBB,
                                                       BasicBlock &SanInsts) {
  auto FoldPhi = [&](PHINode &PHI) {
    Value *IV = nullptr;
    __xsan::compat::removeIncomingValueIf(PHI, [&](const unsigned Idx) -> bool {
      auto *IncomingBB = PHI.getIncomingBlock(Idx);
      bool Remove =
          DetachedEdges.contains(IncomingBB) || IncomingBB == &SourceBB;
      if (Remove && !IV)
        IV = PHI.getIncomingValue(Idx);
      if (IV == ImposedBySan)
        IV = const_cast<Value *>(CondVal);
      return Remove;
    });
    requireOrDie(IV, "[DeSan] Unexpected strange PHI");
    PHI.addIncoming(IV, &SourceBB);
  };
  // Handle PHis
  for (PHINode &PHI : SinkBB->phis())
    FoldPhi(PHI);

  // Handle implict edge SanInsts -> SinkBB
  SanInsts.getTerminator()->replaceSuccessorWith(SinkBB, nullptr);

  // AddRecord does not own SanBBs (which are owned by BlockAddRecord).
  // Therefore, we should not modify DetachedEdges's implict edge at this place;
  // they should be modified at BlockAddRecord::revert.
  // All in all, we cannot add the following code here due to ownership
  // violation:
  // ```
  //   for (auto *Pred : DetachedEdges)
  //     Pred->getTerminator()->replaceSuccessorWith(SinkBB, nullptr);
  // ```
}

static void extractPhiAddSeq(const Sanitizer &San, AddArchive &Archive,
                             BasicBlock &BB) {
  auto PhiRecord = PHIAddRecord::extractFrom(San, BB);
  if (PhiRecord.has_value())
    Archive.PhiRecords.push_back(std::move(*PhiRecord));
}

/// Extract Add Sequence from current user BB.
/// Use mutable @p BIt to record the progress.
/// @return a bool to indicate whether we can further extract add sequence from
/// this BB.
static bool extractInstsAddSeq(const Sanitizer &San, AddArchive &Adds,
                               DeleteArchive &Deletes, BasicBlock &SrcBB,
                               BasicBlock::iterator &BIt, const BBsSet &Users) {
  assert(Users.contains(&SrcBB) &&
         "The source BB of SanAddSeq must be user BB");
  assert(BIt != SrcBB.end());
  // ================================================================
  // 1. SrcBB[SanItBegin:SanItEnd] = SrcBB.nextSplice(isSanSlice)
  // ================================================================
  auto SanItBegin = nextSanInst(BIt, SrcBB.end(), San);
  if (SanItBegin == SrcBB.end())
    return false;
  auto SanItEnd = nextUsrInst(SanItBegin, SrcBB.end(), San);

  if (SanItEnd != SrcBB.end()) {
    // =================================================================
    // 2. Extract pure add sequence if !SourceBB.endswith(SanItEnd)
    // =================================================================
    // Old: BIt ----usr------ SanItBegin ----san---- SanItEnd ---- usr ----
    // New:     --- usr ----                         BIt      ---- usr ----
    Instruction &NextUsr = *SanItEnd;
    Adds.addRecord(AddRecord::extractFrom(SrcBB, SanItBegin, SanItEnd));
    BIt = NextUsr.getIterator();
    return true;
  }

  Instruction *Term = SrcBB.getTerminator();
  assert(Term && isSanInst(San, *Term) && "Unexpect non-San terminator");

  // ================================================================
  // 3. Extract split add sequence if SourceBB.endswith(SanItEnd)
  // ================================================================
  auto MaybeSplit = extractSplitAdd(San, SrcBB, Users);
  if (!MaybeSplit) {
    requireOrDie(
        San.isUBSan(),
        "[DeSan] Strange non-split adds for non-UBSan scenario.\n\t BB: " +
            SrcBB.getName() + "\n\tSan: " + San.name());
    return false;
  }
  auto Split = std::move(*MaybeSplit);
  if (Split.ReuseExistingSink) {
    Adds.addRecord(AddRecord::extractFrom(
        SrcBB, SanItBegin, Split.SinkBB, Split.CondVal, Split.ImposedBySan,
        std::move(Split.SinkSanPreds), std::move(Split.SanBBs)));
    // The br is newly created, should being deleted after add-replaying.
    Deletes.insert(SrcBB.getTerminator());
    return false;
  }

  assert(Split.SinkUserI != nullptr &&
         "desan split add extraction lost its sink user anchor");
  // Old: BIt ----usr------ SanItBegin ----san---- BBEnd
  // New:     --- usr ----                         BIt  ---- usr ----
  Adds.addRecord(AddRecord::extractFrom(SrcBB, SanItBegin, Split.SinkBB,
                                        Split.SinkUserI->getIterator(),
                                        std::move(Split.SanBBs)));
  assert(Split.SinkUserI->getParent() == &SrcBB &&
         "Sink User Inst must be moved to SrcBB");
  BIt = Split.SinkUserI->getIterator();
  return true;
}

static void restoreAdd(const Sanitizer &San, AddArchive &Adds,
                       DeleteArchive &Deletes, Module &M,
                       const UserBBsMap &UserBBsMap) {
  for (Function &F : M) {
    if (F.isDeclaration() ||
        /* UBSan does not support DisableSanitizerInstrumentation */
        !San.isUBSan() &&
            F.hasFnAttribute(
                Attribute::AttrKind::DisableSanitizerInstrumentation))
      continue;

    const auto UserBBsIt = UserBBsMap.find(&F);
    requireOrDie(UserBBsIt != UserBBsMap.end(),
                 "Unexpected non-user function found: " + F.getName());
    const BBsSet &UserBBs = UserBBsIt->second;

    // ================================================================
    //    1. Extract SanInsts from BB and move UserInsts (BBs stable)
    // ================================================================
    for (BasicBlock &BB : F) {
      if (!UserBBs.contains(&BB))
        continue;

      auto BIt = compat::getFirstNonPHIIt(&BB)->getIterator();
      while (BIt != BB.end() &&
             extractInstsAddSeq(San, Adds, Deletes, BB, BIt, UserBBs))
        ;
    }

    // ================================================================
    //    2. Extract SanPHIs from the UserBB PHI list (CFG stable).
    // ================================================================
    for (BasicBlock &BB : F)
      if (UserBBs.contains(&BB))
        extractPhiAddSeq(San, Adds, BB);

    // =================================================================
    //    3. Extract detached SanBBs from (break BB lists).
    // ================================================================
    BlockAddRecord::SanBBVec PendingSanBBs;
    /// TODO: figure out should we maintain the insert point for SanBBs? Why not
    /// insert them into F.end directly?
    auto FlushPending = [&](BasicBlock *InsertPt) {
      if (PendingSanBBs.empty())
        return;
      BlockAddRecord BBRecord{&F, PendingSanBBs, InsertPt};
      BBRecord.revert();
      Adds.BBRecords.push_back(std::move(BBRecord));
      PendingSanBBs.clear();
    };
    // For UBSan, some SanBBs might not be eliminated. Those special SanBBs
    // would be treated as User BB in the subsequent DeSan. Therefore, we remove
    // those unreachable BBs as `BlockAddRecord`s.
    const auto &&ReachableBBs =
        San.isUBSan() ? SmallSetVector<const BasicBlock *, 16>{llvm::from_range,
                                                               depth_first(&F)}
                      : SmallSetVector<const BasicBlock *, 16>{};
    auto IsReachable = [isUBSan = San.isUBSan(), &ReachableBBs,
                        &UserBBs](const BasicBlock *BB) {
      return isUBSan ? ReachableBBs.contains(BB) : UserBBs.contains(BB);
    };
    auto FindUserSucc = [&](const BasicBlock &BB) {
      auto Successors = successors(&BB);
      auto It = find_if(Successors, IsReachable);
      return It == Successors.end() ? nullptr : const_cast<BasicBlock *>(*It);
    };
    for (BasicBlock &BB : F) {
      if (IsReachable(&BB))
        FlushPending(&BB);
      else
        PendingSanBBs.emplace_back(&BB, FindUserSucc(BB));
    }
    FlushPending(nullptr);
  }
  if (!San.isUBSan())
    Adds.tagAs(San);
}

struct UbsanReplaceRule {
  Instruction::BinaryOps Opcode;
  bool HasNoSignedWrap;
};

static bool getUbsanReplaceRule(Intrinsic::ID ID, UbsanReplaceRule &Rule) {
  switch (ID) {
  case Intrinsic::sadd_with_overflow:
    Rule = {Instruction::Add, true};
    return true;
  case Intrinsic::uadd_with_overflow:
    Rule = {Instruction::Add, false};
    return true;
  case Intrinsic::ssub_with_overflow:
    Rule = {Instruction::Sub, true};
    return true;
  case Intrinsic::usub_with_overflow:
    Rule = {Instruction::Sub, false};
    return true;
  case Intrinsic::smul_with_overflow:
    Rule = {Instruction::Mul, true};
    return true;
  case Intrinsic::umul_with_overflow:
    Rule = {Instruction::Mul, false};
    return true;
  default:
    return false;
  }
}

static bool allUsesAreUbsan(const Value &V) {
  return all_of(V.users(), [](const User *U) {
    return isa<Instruction>(U) && __xsan::UBSanInst::is(*cast<Instruction>(U));
  });
}

static bool hasTaggedUbsanOverflowExtract(const WithOverflowInst &WOI) {
  return llvm::any_of(WOI.users(), [](const User *U) {
    const auto *EI = dyn_cast<ExtractValueInst>(U);
    return EI && EI->getNumIndices() == 1 && EI->getIndices()[0] == 1 &&
           __xsan::UBSanInst::is(*EI);
  });
}

static WithOverflowInst *matchUBSanReplacement(Instruction &I,
                                               UbsanReplaceRule &Rule) {
  using namespace llvm::PatternMatch;

  WithOverflowInst *WOI = nullptr;
  if (!match(&I, m_ExtractValue<0>(m_WithOverflowInst(WOI))))
    return nullptr;
  // This I has no user uses.
  if (UBSanInst::is(I))
    return nullptr;
  if (!getUbsanReplaceRule(WOI->getIntrinsicID(), Rule))
    return nullptr;
  if (!hasTaggedUbsanOverflowExtract(*WOI))
    return nullptr;
  return WOI;
}

static Instruction *foldUBSanReplacement(ExtractValueInst &Replacer,
                                         WithOverflowInst &WithOverflow,
                                         const UbsanReplaceRule &Rule) {
  Twine Name = Replacer.hasName() ? Replacer.getName() : "";

  auto *Replacee =
      BinaryOperator::Create(Rule.Opcode, WithOverflow.getArgOperand(0),
                             WithOverflow.getArgOperand(1), Name);
  if (Rule.HasNoSignedWrap)
    Replacee->setHasNoSignedWrap();

  __xsan::compat::insertInstructionBefore(Replacee, &WithOverflow);
  Replacee->setDebugLoc(Replacer.getDebugLoc());
  __xsan::UserIR::set(*Replacee);
  return Replacee;
}

static void restoreUbsanArithmetic(const Sanitizer &San, ReSanArchive &Archive,
                                   Module &M) {
  struct UbsanArithmeticReplacement {
    ExtractValueInst *Replacer;
    WithOverflowInst *WithOverflow;
    UbsanReplaceRule Rule;
    Instruction *Replacee;
  };

  // Collect target replacements
  SmallVector<UbsanArithmeticReplacement, 16> Worklist;
  for (Function &F : M) {
    for (Instruction &I : instructions(F)) {
      UbsanReplaceRule Rule;
      if (auto *WOI = matchUBSanReplacement(I, Rule))
        Worklist.push_back({cast<ExtractValueInst>(&I), WOI, Rule, nullptr});
    }
  }

  // Restore deletion
  for (auto &[Replacer, WOI, Rule, Replacee] : Worklist) {
    Replacee = foldUBSanReplacement(*Replacer, *WOI, Rule);
    requireOrDie(Replacee, "desan ubsan arithmetic restore lost its pattern");
    Archive.DeleteRecords.insert(Replacee);
  }

  // Restore replacement
  for (auto &[Replacer, WOI, _, Replacee] : Worklist) {
    const bool RealReplace = replaceNonSanInstUses(San, *Replacer, *Replacee);
    if (allUsesAreUbsan(*Replacer)) {
      __xsan::UBSanInst::set(*Replacer);
      __xsan::UserIR::drop(*Replacer);
    }
    if (allUsesAreUbsan(*WOI)) {
      __xsan::UBSanInst::set(*WOI);
      __xsan::UserIR::drop(*WOI);
    }
    Archive.ReplaceRecords[Replacee].push_back({Replacer, RealReplace});
  }
}

void runDeSan(Module &M, SanitizerArchive &Entry, UserBBsMap &UserBBsMap) {
  const Sanitizer &San = Entry.San;
  ReSanArchive &Archive = Entry.Archive;

  if (San.isUBSan()) {
    // UBSan is not enabled.
    if (!__xsan::hasUbsanFunction(M))
      return;
    // UBSan performs sanitizing instrumentation on compiler-frontend, and thus
    // might be affected by optimization. We need to specialize the handling of
    // UBSan.
    restoreUbsanArithmetic(San, Archive, M);
    auto UBSanUserBBsMap = collectUserBBsForUBSan(M);
    restoreAdd(San, Archive.AddRecords, Archive.DeleteRecords, M,
               UBSanUserBBsMap);
    return;
  }

  const auto &DeleteEvents = getDeleteEvents();
  const auto &ReplaceEvents = getReplaceEvents();

  restoreFunction(San, Archive.FunctionRecords, M, UserBBsMap);
  restoreDelete(San, Archive.DeleteRecords, DeleteEvents);
  restoreReplace(San, Archive.ReplaceRecords, ReplaceEvents);
  restoreAdd(San, Archive.AddRecords, Archive.DeleteRecords, M, UserBBsMap);

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS((llvm::Twine(San.name()) + ".archive.txt").str(),
                            EC, llvm::sys::fs::OF_None);
    dumpArchive(San, Archive, OS);
  }
}

} // namespace __xsan::desan
