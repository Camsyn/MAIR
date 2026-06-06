//===----------------------------------------------------------------------===//
// ReSan workflow
//===----------------------------------------------------------------------===//
//
// ReSan is the replay half of DeSan. After all sanitizer passes have run on a
// DeSan-restored user IR, ReSan consumes the accumulated ReSanArchive entries
// and reconstructs the sanitizer semantics on the final module.
//
// 1. Validate replace archive: fail before replay mutates IR.
//    1.1. Each sanitizer archive may have at most one replacement record for
//         one replacee.
//    1.2. The check runs before Add replay because Add replay can move anchors
//         and split user BBs.
//
// 2. replayAdd / prepare replacer AddRecords: classify AddRecords that contain
//    replacement values.
//    2.1. Build detached SanBB -> AddRecord ownership.
//    2.2. Put replacer-after-replacee records before normal Add replay.
//    2.3. Retarget detached sanitizer uses so SanUse(replacee) is replayed
//         after the replacer when required.
//    2.4. Keep replacer-before-replacee records until after normal Add replay.
//    2.5. Default UBSan arithmetic replacements are not AddRecords; replay
//         them later through replayReplace/replayDelete.
//
// 3. replayAdd / PHIs: replay all PHIAddRecords first.
//    3.1. Insert SanPHIs back into their target user BB PHI lists.
//    3.2. Do this before SanBB replay so all user BB sanitizer PHIs exist
//         before CFG edges are restored.
//
// 4. replayAdd / SanBBs: replay all BlockAddRecords next.
//    4.1. Reattach sanitizer-only BBs to their parent functions.
//    4.2. Do not move user instructions in this phase.
//
// 5. replayAdd / SanInsts: replay normal AddRecords last.
//    5.1. Replay replacer-after-replacee AddRecords first.
//    5.2. Replay each sanitizer's remaining AddRecords in sanitizer order.
//    5.3. Replay replacer-before-replacee AddRecords at the end.
//    5.4. AddRecord owns Pure/Split/Redirect local CFG and PHI repair.
//
// 6. replayReplace: restore replacement semantics.
//    6.1. Assumption: after replayAdd, each replacer dominates every
//         replacee use that ReSan is allowed to rewrite.
//    6.2. Skip sanitizer-owned uses for ordinary sanitizers.
//    6.3. Let UBSan use its frontend-recovered broader replay exception.
//
// 7. replayDelete: erase user instructions that DeSan restored only for replay.
//    7.1. Delete temporary anchors and replacees after Add and Replace replay.
//    7.2. Do not require use_empty() because one deleted instruction may use
//         another deleted instruction.
//
// 8. replayFunction: reattach sanitizer-only functions after instruction
//    replay.
//    8.1. Push archived functions back into the module function list.
//
// AddRecord, PHIAddRecord, and BlockAddRecord own their local replay mechanics;
// the top-level pass only enforces this global archive order.
//
//===----------------------------------------------------------------------===//

#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"
#include "XSanDeSanReSan.h"
#include "XSanSanitizingContext.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <functional>
#include <string>

using namespace llvm;

namespace __xsan::desan {

void BlockAddRecord::replay(Module &M) {
  if (Replayed)
    return;

  Function *F = Parent;
  BasicBlock *InsertBefore = InsertPt;
  requireOrDie(F, "resan add replay lost parent function");
  if (F->getParent() != &M)
    return;
  if (InsertBefore && InsertBefore->getParent() != F)
    return;

  for (auto [SanBB, UserSucc] : SanBBs) {
    assert(SanBB && "resan add replay lost a sanitizer block");
    requireOrDie(
        !SanBB->getParent(),
        "resan add replay found a sanitizer block already in a function");
    SanBB->insertInto(F, InsertBefore);
    auto *Term = SanBB->getTerminator();
    if (UserSucc && Term)
      Term->replaceSuccessorWith(nullptr, UserSucc);
  }
  Replayed = true;
}

void PHIAddRecord::replay() {
  requireOrDie(TargetUsrBB, "resan phi add replay lost target user block");
  auto InsertPt = TargetUsrBB->begin();

  for (PHINode *Phi : SanPhis) {
    assert(Phi && "resan phi add replay lost phi");
    assert(!Phi->getParent() &&
           "resan phi add replay found phi already in a block");
    compat::insertInstructionBefore(Phi, InsertPt);
  }
}

Instruction &AddRecord::replayPure() {
  Instruction &Anchor = *this->Anchor;
  BasicBlock *SrcUserBB = Anchor.getParent();
  requireOrDie(SrcUserBB, "resan add replay get anchor without a parent block");
  requireOrDie(!SanInsts->empty(),
               "resan split add replay got empty sanitizer instructions");
  Instruction &SanBeg = *SanInsts->begin();
  // SrcBB' = SrcBB[:Anchor] : SanInsts : SrcBB[Anchor:]
  __xsan::compat::spliceInstructions(SrcUserBB, Anchor.getIterator(),
                                     SanInsts.get());
  return SanBeg;
}

void AddRecord::replayPHiUses() {
  const SplitExtra *Split;
  switch (Kind) {
  case AddRecordKind::Split:
    Split = std::get_if<AddRecord::SplitExtra>(&this->ExtraRecord);
    break;
  case AddRecordKind::Redirect:
    Split = std::get_if<AddRecord::RedirectExtra>(&this->ExtraRecord);
    break;
  default:
    Split = nullptr;
  }
  requireOrDie(Split, "Cannot replay phi uses for non-split add record");
  // Split AddRecord is extracted from OldSrc -> ... -> Sink
  // Now we are replaying this record into NewSrc, causing NewSrc -> ... -> Sink
  BasicBlock *Sink = Split->SinkBB, *OldSrc = Split->SrcBB,
             *NewSrc = Anchor->getParent();
  assert(NewSrc != Sink && "This function should be called before split");
  // Update the entry (OldSrc -> NewSrc) in SanBBs' phis
  for (auto *SanBB : SanBBs)
    SanBB->replacePhiUsesWith(OldSrc, NewSrc);

  if (Kind == AddRecordKind::Redirect)
    return;
  // Only strict split can do successors' phi updates.

  // Update (NewSrc -> Sink) in phis of Sink's successors
  for (BasicBlock *Succ : successors(NewSrc))
    Succ->replacePhiUsesWith(NewSrc, Sink);
}

void AddRecord::replaySplit() {
  Instruction &Anchor = *this->Anchor;
  BasicBlock *SrcUserBB = Anchor.getParent();
  requireOrDie(SrcUserBB, "resan add replay get anchor without a parent block");
  auto *Split = std::get_if<AddRecord::SplitExtra>(&ExtraRecord);
  assert(Split && "unreachable code in resan add replay record");
  replayPHiUses();
  // Replay as pure add at first.
  replayPure();

  // Move SrcBB[Anchor:] as SinkBB's suffix
  // SrcBB'  = SrcBB[:Anchor]
  // SinkBB' = SinkBB[:] : SrcBB[Anchor:]
  __xsan::compat::spliceInstructions(Split->SinkBB, Split->SinkBB->end(),
                                     SrcUserBB, Anchor.getIterator(),
                                     SrcUserBB->end());
  assert(Anchor.getParent() == Split->SinkBB);
}

void AddRecord::RedirectExtra::replayEdgesOfSanBB2Sink(BasicBlock &SrcBB,
                                                       BasicBlock &SanInsts) {
  // If there is no null successor in SanInsts. it means SrcBB is not a
  // predecessor of SinkBB.
  assert(SanInsts.getTerminator() &&
         "This step should be done before addition replay");
  const bool IsSrcAsPred = llvm::is_contained(successors(&SanInsts), nullptr);
  // Recover hidden edge SanBB -> SinkBB
  if (IsSrcAsPred)
    SanInsts.getTerminator()->replaceSuccessorWith(nullptr, SinkBB);
  // AddRecord does not own SanBBs (which are owned by BlockAddRecord).
  // Therefore, we should not modify DetachedEdges's implict edge at this place;
  // they should be modified at BlockAddRecord::replay.
  // All in all, we cannot add the following code here due to ownership
  // violation:
  // ```
  // for (auto *Pred : DetachedEdges)
  //   Pred->getTerminator()->replaceSuccessorWith(nullptr, SinkBB);
  // ```
  for (PHINode &PHI : SinkBB->phis()) {
    const int Idx = PHI.getBasicBlockIndex(&SrcBB);
    requireOrDie(Idx >= 0, "ReSan redirect replay lost folded source PHI");
    Value *IV = PHI.getIncomingValue(Idx);
    if (!IsSrcAsPred)
      PHI.removeIncomingValue(Idx);
    for (auto *Detached : DetachedEdges)
      PHI.addIncoming(IV, Detached);
  }
}

void AddRecord::replayRedirect() {
  Instruction &Anchor = *this->Anchor;
  BasicBlock *SrcUserBB = Anchor.getParent();
  requireOrDie(SrcUserBB, "ReSan add replay get anchor without a parent block");
  auto *Redirect = std::get_if<AddRecord::RedirectExtra>(&ExtraRecord);
  assert(Redirect && "unreachable code in resan add replay record");
  // Insert detached IVs for PHIs in SinkBB. Reverse operation of
  // foldSinkPhiIVs.
  Redirect->replayEdgesOfSanBB2Sink(*SrcUserBB, *SanInsts);
  replayPHiUses();
  // Replay as pure add at first.
  auto &SanBeg = replayPure();
  // This 'user' br was creared by previous DeSan.
  // However, addition replay should not remove any inst.
  // Move this before the SanInsts to avoid any other SanInsts being inserted
  // after a san-br.
  compat::moveBefore(&Anchor, &SanBeg);
}

void AddRecord::replay() {
  if (!disable() || Replayed)
    return;
  switch (Kind) {
  case AddRecordKind::Pure:
    replayPure();
    break;
  case AddRecordKind::Split:
    replaySplit();
    break;
  case AddRecordKind::Redirect:
    replayRedirect();
    break;
  default:
    llvm_unreachable("Unreachable add record Kind");
  }
  Replayed = true;
}

static bool mustComesBefore(const Value &LHS, const AddRecord &RHS) {
  const Instruction &RHSAnchor = *RHS.Anchor;
  if (auto *Arg = dyn_cast<Argument>(&LHS))
    return Arg->getParent() == RHSAnchor.getFunction();

  const auto *LHSAnchor = dyn_cast<Instruction>(&LHS);
  if (!LHSAnchor || LHSAnchor->getParent() != RHSAnchor.getParent())
    return false;
  // If LHS and RHS in different BB, LHS.comesBefore(RHS) is UB!
  return LHSAnchor->comesBefore(&RHSAnchor);
}

static bool mustComesBefore(const AddRecord &LHS, const AddRecord &RHS) {
  return mustComesBefore(*LHS.Anchor, RHS);
}

namespace {
/// Take over the replay of AddRecord if it is a replacer.
struct ReplacerAddRecord {
  std::reference_wrapper<AddRecord> Replacer;
  ReplacerAddRecord(AddRecord &Add) : Replacer(Add) { Add.disable(); }

  void replay() {
    Replacer.get().enable();
    Replacer.get().replay();
  }
};
} // namespace

/// Prepare late replacements before normal add replay.
/// 1. Identify those replacer after its replacee.
/// 2. Change the anchor of those @ref AddRecords, which contain
/// SanUse(replacee)
///    before the replacer, as the anchor of the replacer.
///
/// @return list of @ref ReplacerAddRecord which contains replacer after its
/// replacee. This list should be used to replay at first to make sure any
/// SanUse(replacee) is inserted after replacer.
static void obtainReplacersAndAdjustSanUseAnchor(
    SanitizingContext &Context,
    SmallVectorImpl<ReplacerAddRecord> &AfterReplacers,
    SmallVectorImpl<ReplacerAddRecord> &BeforeReplacers) {
  // =====================================================
  // 1. Build BB -> AddRecord owner map
  // =====================================================

  DenseMap<const BasicBlock *, AddRecord *> BB2Add;
  const unsigned NumAddRecords = __xsan::compat::sumOf(llvm::map_range(
      Context.SanitizerArchives, [](const SanitizerArchive &Entry) {
        return Entry.Archive.AddRecords.Records.size();
      }));

  BB2Add.reserve(NumAddRecords * 2);
  auto BindBB2Add = [&BB2Add](const BasicBlock &BB, AddArchive &Archive,
                              AddRecord &Record) {
    auto It = BB2Add.find(&BB);
    if (It == BB2Add.end())
      return (void)(BB2Add[&BB] = &Record);
    // A shared SanBB belongs to more than one AddRecord. Such a block is not a
    // precise owner for a replacement instruction, so keep the ownership
    // ambiguous and fail fast if a late replacement ever depends on it.
    if (It->second != &Record)
      BB2Add.erase(It);
  };
  auto Inst2Add = [&BB2Add](const Instruction &I) -> AddRecord * {
    auto It = BB2Add.find(I.getParent());
    requireOrDie(It != BB2Add.end(),
                 "resan add cannot find replacement add record");
    return It->second;
  };

  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    AddArchive &Archive = Entry.Archive.AddRecords;
    std::vector<AddRecord> &Records = Archive.Records;
    for (size_t I = 0, E = Records.size(); I != E; ++I) {
      AddRecord &Record = Records[I];
      BindBB2Add(*Record.SanInsts.get(), Archive, Record);
      for (BasicBlock *SanBB : Record.getSanBBs())
        BindBB2Add(*SanBB, Archive, Record);
    }
  }

  // =====================================================
  // 2. Find after-replacers and retarget san_use anchors
  // =====================================================

  const unsigned NumReplacement = __xsan::compat::sumOf(llvm::map_range(
      Context.SanitizerArchives, [](const SanitizerArchive &Entry) {
        return Entry.Archive.ReplaceRecords.size();
      }));
  AfterReplacers.reserve(NumReplacement);
  BeforeReplacers.reserve(NumReplacement);
  for (const SanitizerArchive &Entry : Context.SanitizerArchives) {
    for (const auto &[Replacee, Records] : Entry.Archive.ReplaceRecords) {
      assert(Replacee && Records.size() == 1 &&
             "Unexpected replacement occurs");

      // Find replacer AddRecord.
      auto *Replacer = dyn_cast<Instruction>(Records.front().Replacer);
      if (!Replacer)
        continue;
      assert(
          Replacer->getParent() &&
          (!Replacer->getParent()->getParent() || UBSanInst::is(*Replacer)) &&
          "Non-UBSan replacer must belong to some detached san block");
      AddRecord &ReplacerRecord = *Inst2Add(*Replacer);

      if (!mustComesBefore(*Replacee, ReplacerRecord)) {
        // This replacer is before the replacee
        BeforeReplacers.push_back(ReplacerRecord);
        continue;
      }
      // This replacer is after the replacee
      AfterReplacers.push_back(ReplacerRecord);
      // Move detached san_use(replacee) AddRecords after the replacer.
      for (User *U : Replacee->users()) {
        auto *SanUse = dyn_cast<Instruction>(U);
        // Keep only detached sanitizer uses.
        if (!SanUse || !SanUse->getParent() || SanUse->getParent()->getParent())
          continue;
        AddRecord &SanUseRecord = *Inst2Add(*SanUse);
        if (&SanUseRecord == &ReplacerRecord ||
            SanUseRecord.Anchor == ReplacerRecord.Anchor)
          continue;
        if (mustComesBefore(SanUseRecord, ReplacerRecord))
          SanUseRecord.retargetAnchor(*ReplacerRecord.Anchor);
      }
    }
  }
}

/// Replay the add records as follows:
/// 1. Replay additions of PHIs in user BBs
/// 2. Replay additions of SanBB (not affect UserBB)
/// 3. Replay additions of SanInsts (might split UserBB and move some user
///    instructions)
///    3-1: If the additions are related to replacer after replacee, replay them
///         at first.
///    3-2: Replay other additions in order.
static void replayAdd(Module &M, SanitizingContext &Context,
                      unsigned DumpIndex) {
  // =========================================================
  // 0. Get AddRecords of after-replacer and adjust SanUse
  // =========================================================
  SmallVector<ReplacerAddRecord, 8> AfterReplacers, BeforeReplacers;
  obtainReplacersAndAdjustSanUseAnchor(Context, AfterReplacers,
                                       BeforeReplacers);

  // =========================================================
  // 1. Replay all PHIs on UserBBs
  // =========================================================

  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    AddArchive &Archive = Entry.Archive.AddRecords;
    for (PHIAddRecord &Record : Archive.PhiRecords)
      Record.replay();
  }

  Context.dumpNestedIR(M, DumpIndex, "1-0-resan-add-phi");

  // =========================================================
  // 2. Replay all SanBBs
  // =========================================================

  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    AddArchive &Archive = Entry.Archive.AddRecords;
    for (BlockAddRecord &Record : Archive.BBRecords)
      Record.replay(M);
  }

  Context.dumpNestedIR(M, DumpIndex, "1-1-resan-add-bb");

  // =========================================================
  // 2. Replay all SanInsts
  // =========================================================

  // We need to firstly replay those add record which is treated as
  // after-replacer.
  // This make sure any SanUse(replacee) is after the replacer.
  //
  // Sanitizing Assumption:
  //   if replacer is after replacee, any use(replacee) should be, or could be
  //   safely moved, after replacer's anchor.
  for (auto &AfterReplacerRecord : AfterReplacers) {
    AfterReplacerRecord.replay();
  }
  Context.dumpNestedIR(M, DumpIndex, "1-2-1-resan-add-after-replacer");
  // Then replay SanInsts in order. The replay with idempotence can be repeated
  // (only the first effects).
  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    AddArchive &Archive = Entry.Archive.AddRecords;
    const Sanitizer &San = Entry.San;
    for (AddRecord &Record : Archive.Records)
      Record.replay();
    Context.dumpNestedIR(M, DumpIndex,
                         "1-2-2-" + std::to_string(San.order()) +
                             "-resan-add-inst-" + San.name());
  }

  // We need to eventually replay those add record which is treated as
  // before-replacer.
  // This make sure any AddSeq before the replacee is also before the replacer.
  //
  // Sanitizing Assumption:
  //   if replacer is before replacee, its anchor must be replacee.
  for (auto &BeforeReplacerRecord : BeforeReplacers)
    BeforeReplacerRecord.replay();

  Context.dumpNestedIR(M, DumpIndex, "1-2-3-resan-add-before-replacer");
  Context.dumpNestedIR(M, DumpIndex, "1-2-resan-add-inst");
}

/// We only replace replacee -> replacer on cases:
/// 1. non-instruction use
/// 2. non-current San's use
static bool shouldReplayReplacement(const Sanitizer &San, Use &U) {
  auto *Inst = dyn_cast<Instruction>(U.getUser());
  return !Inst || !San.is(*Inst);
}

/// We do this replay based on the following assumption (guaranteed by
/// Sanitizing Semantic):
///      replacer dom all_use(replacee)
static void replayReplace(Module &M, SanitizingContext &Context) {
  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    for (auto &[Replacee, Records] : Entry.Archive.ReplaceRecords) {
      requireOrDie(Records.size() == 1,
                   Twine("resan found multiple replace records in sanitizer ") +
                       Entry.San.name());
      // The replacement is in logic, and no real replacement happens.
      if (Replacee->use_empty())
        continue;
      Value *Replacer = Records.front().Replacer;
      assert(Replacer && "Unexpected null replacer");
      if (Entry.San.isUBSan())
        Replacee->replaceAllUsesWith(Replacer);
      else
        Replacee->replaceUsesWithIf(Replacer, [&](Use &U) {
          // Sanitizing Assumption:
          //   replayAdd has placed Replacer so it dominates every replacee use
          //   that ReSan is allowed to rewrite.
          return shouldReplayReplacement(Entry.San, U);
        });
    }
  }
}

static void replayDelete(Module &M, SanitizingContext &Context) {
  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    for (Instruction *UserInst : Entry.Archive.DeleteRecords) {
      assert(UserInst && UserInst->getParent() && "Unexpected Inst to Remove");
      // Do not check use_empty() as one Inst2Remove might be used by another
      // Inst2Remove, which might not use Replacer when being deleted.
      UserInst->eraseFromParent();
    }
  }
}

static void replayFunction(Module &M, SanitizingContext &Context) {
  for (SanitizerArchive &Entry : Context.SanitizerArchives)
    for (Function *F : Entry.Archive.FunctionRecords)
      M.getFunctionList().push_back(F);
}

static void validateReplaceArchive(const Sanitizer &San,
                                   const ReplaceArchive &Archive) {
  for (auto &[UserValue, Records] : Archive) {
    if (Records.size() > 1) {
      std::string S;
      llvm::raw_string_ostream OS(S);
      UserValue->print(OS);
      OS << "\nMultiple records in sanitizer: " << San.name() << "\n";
      report_fatal_error(OS.str().c_str());
    }
  }
}

void runReSan(Module &M, SanitizingContext &Context, unsigned DumpIndex) {
  for (const SanitizerArchive &Entry : Context.SanitizerArchives)
    validateReplaceArchive(Entry.San, Entry.Archive.ReplaceRecords);

  replayAdd(M, Context, DumpIndex);
  replayReplace(M, Context);
  Context.dumpNestedIR(M, DumpIndex, "2-resan-replace");
  replayDelete(M, Context);
  Context.dumpNestedIR(M, DumpIndex, "3-resan-delete");
  replayFunction(M, Context);
  Context.dumpNestedIR(M, DumpIndex, "4-resan-func");
}

ReSanPass::ReSanPass(std::shared_ptr<SanitizingContext> Ctx)
    : Ctx(std::move(Ctx)) {}

PreservedAnalyses ReSanPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!Ctx, "resan replay pass missing pipeline context");

  if (options::ClDebug)
    for (const SanitizerArchive &Entry : Ctx->SanitizerArchives)
      validateArchive(Entry.San, Entry.Archive, Ctx->UserBBs);

  unsigned DumpIndex = Ctx->reserveDumpIndex();
  runReSan(M, *Ctx, DumpIndex);
  Ctx->dumpIR(M, DumpIndex, "resan");

  if (options::ClDebug)
    requireOrDie(!llvm::verifyModule(M, &llvm::errs()),
                 "resan replay produced invalid IR");

  return PreservedAnalyses::none();
}

} // namespace __xsan::desan
