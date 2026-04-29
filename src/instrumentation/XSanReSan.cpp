#include "Utils/Options.h"
#include "XSanDeSanReSan.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace __xsan::desan {

namespace {

struct ReplaceRef {
  const Sanitizer *San = nullptr;
  const ReplaceRecord *Record = nullptr;
};

struct AddRecordRef {
  const Sanitizer *San = nullptr;
  AddRecord *Record = nullptr;
};

static bool isCandidateSanitizer(const Sanitizer &Candidate,
                                 const Sanitizer *Base, bool IncludeBase) {
  if (!Base)
    return true;
  if (IncludeBase && Candidate.sameAs(*Base))
    return true;
  return Candidate.isLaterThan(*Base);
}

static ReplaceRef findLatestReplaceRecord(const SanitizingContext &Context,
                                          Value *UserValue,
                                          const Sanitizer *Base,
                                          bool IncludeBase) {
  ReplaceRef Selected;
  for (const SanitizerArchive &Entry : Context.SanitizerArchives) {
    if (!isCandidateSanitizer(Entry.San, Base, IncludeBase))
      continue;

    auto It = Entry.Archive.ReplaceRecords.find(UserValue);
    if (It == Entry.Archive.ReplaceRecords.end())
      continue;
    requireOrDie(It->second.size() == 1,
                 Twine("resan found multiple replace records in sanitizer ") +
                     Entry.San.name());

    if (!Selected.Record || Entry.San.isLaterThan(*Selected.San))
      Selected = {&Entry.San, &It->second.front()};
  }
  return Selected;
}

static ReplaceRef findLatestOtherReplaceRecord(const SanitizingContext &Context,
                                               Value *UserValue,
                                               const Sanitizer &San) {
  ReplaceRef Selected;
  for (const SanitizerArchive &Entry : Context.SanitizerArchives) {
    if (Entry.San.sameAs(San))
      continue;

    auto It = Entry.Archive.ReplaceRecords.find(UserValue);
    if (It == Entry.Archive.ReplaceRecords.end())
      continue;
    requireOrDie(It->second.size() == 1,
                 Twine("resan found multiple replace records in sanitizer ") +
                     Entry.San.name());

    if (!Selected.Record || Entry.San.isLaterThan(*Selected.San))
      Selected = {&Entry.San, &It->second.front()};
  }
  return Selected;
}

static bool isDeletedByLaterSanitizer(const SanitizingContext &Context,
                                      Instruction &I, const Sanitizer &Base) {
  for (const SanitizerArchive &Entry : Context.SanitizerArchives) {
    if (!Entry.San.isLaterThan(Base))
      continue;
    if (Entry.Archive.DeleteRecords.find(&I) !=
        Entry.Archive.DeleteRecords.end())
      return true;
  }
  return false;
}

static Value *resolveReplayReplacement(const SanitizingContext &Context,
                                       const Sanitizer &San,
                                       Value *SanitizerValue) {
  if (!SanitizerValue)
    return nullptr;

  SmallPtrSet<Value *, 8> Visited;
  Value *CurrentValue = SanitizerValue;
  const Sanitizer *CurrentSan = &San;

  while (Visited.insert(CurrentValue).second) {
    ReplaceRef Next = findLatestReplaceRecord(Context, CurrentValue, CurrentSan,
                                              /*IncludeBase=*/false);
    if (Next.Record && Next.Record->SanitizerValue) {
      CurrentValue = Next.Record->SanitizerValue;
      CurrentSan = Next.San;
      continue;
    }

    if (auto *I = dyn_cast<Instruction>(CurrentValue))
      if (isDeletedByLaterSanitizer(Context, *I, *CurrentSan))
        return nullptr;

    return CurrentValue;
  }

  return nullptr;
}

static SmallVector<Instruction *, 8>
collectArgumentReplaceInsts(const SanitizingContext &Context, Function &F) {
  SmallVector<Instruction *, 8> ReplaceInsts;
  for (Argument &Arg : F.args()) {
    for (const SanitizerArchive &Entry : Context.SanitizerArchives) {
      auto It = Entry.Archive.ReplaceRecords.find(&Arg);
      if (It == Entry.Archive.ReplaceRecords.end())
        continue;
      for (const ReplaceRecord &Record : It->second)
        if (auto *Inst = dyn_cast<Instruction>(Record.SanitizerValue))
          ReplaceInsts.push_back(Inst);
    }
  }
  return ReplaceInsts;
}

} // namespace

// We need ReplaceArchive to redirect instruction anchors.
static void replayAdd(Module &M, SanitizingContext &Context) {
  // Some sanitizers replace arguments with sanitized versions and assume these
  // replacements are prepared at the beginning of the function. The sentry
  // anchor gives those argument replacements a stable position before the first
  // non-alloca entry instruction and before other sanitizer add records.
  struct FunctionHeaderInfo {
    SmallPtrSet<AllocaInst *, 8> AllocaToBeRedirected;
    Instruction *SentryAnchor;
    // We simplify the logic checking whether an add record contains argument
    // replacement by recording all basic blocks containing these replacements.
    // Should only contain BBs in add records.
    SmallPtrSet<BasicBlock *, 8> BBsContainingReplace;
  };
  DenseMap<Function *, FunctionHeaderInfo> FunctionHeaderMap;
  SmallVector<Instruction *, 8> SentryAnchors;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    // Collect instructions replacing arguments in this function.
    SmallVector<Instruction *, 8> ReplaceInsts =
        collectArgumentReplaceInsts(Context, F);
    if (ReplaceInsts.empty())
      continue;

    auto &EntryBB = F.getEntryBlock();
    auto &HeaderInfo = FunctionHeaderMap[&F];

    // Collect basic blocks containing these replacements.
    for (Instruction *Inst : ReplaceInsts)
      HeaderInfo.BBsContainingReplace.insert(Inst->getParent());

    // Build the sentry anchor and collect the alloca to be redirected.
    auto It = llvm::find_if(
        EntryBB, [](const Instruction &I) { return !isa<AllocaInst>(I); });
    requireOrDie(It != EntryBB.end(), "resan add replay found a BB without a "
                                      "non-alloca terminator instruction");
    auto *DoNothingFunc =
        __xsan::compat::getOrInsertDeclaration(&M, Intrinsic::donothing);
    HeaderInfo.SentryAnchor = CallInst::Create(DoNothingFunc);
    __xsan::compat::insertInstructionBefore(HeaderInfo.SentryAnchor, &*It);
    SentryAnchors.push_back(HeaderInfo.SentryAnchor);
    for (Instruction &I : llvm::make_range(
             EntryBB.begin(), HeaderInfo.SentryAnchor->getIterator()))
      HeaderInfo.AllocaToBeRedirected.insert(cast<AllocaInst>(&I));
  }

  // Some add records' anchors may need to be redirected.
  // 1. If the sanitizer part contains an instruction replacing the argument, we
  //    should redirect to the header sentry mentioned above.
  // 2. If the anchor is an alloca instruction at the function header, we should
  //    redirect to the instruction after the header sentry mentioned above.
  // 3. If anchor is replaced by a later sanitizer, we should redirect to the
  //    new one. Because the add anchor means the add record should be before
  //    the anchor.
  const auto RedirectAnchor =
      [&Context, &FunctionHeaderMap](const Sanitizer &San,
                                     const AddRecord &Record) -> Instruction * {
    auto *F = Record.Anchor->getFunction();
    requireOrDie(F, "resan add replay get anchor without a parent function");
    if (auto It = FunctionHeaderMap.find(F); It != FunctionHeaderMap.end()) {
      auto &HeaderInfo = It->second;

      if (auto *Pure = std::get_if<AddRecord::PureRecord>(&Record.Record)) {
        if (HeaderInfo.BBsContainingReplace.contains(Pure->SanInsts.get()))
          return HeaderInfo.SentryAnchor;
      }
      if (auto *Split = std::get_if<AddRecord::SplitRecord>(&Record.Record)) {
        // We only check SanInstsToAppend and LastBB because we can't track each
        // sanitizer BB belonging to which add record for now.
        if (HeaderInfo.BBsContainingReplace.contains(
                Split->SanInstsToAppend.get()) ||
            HeaderInfo.BBsContainingReplace.contains(Split->LastBB))
          return HeaderInfo.SentryAnchor;
      }

      if (auto *Alloca = dyn_cast<AllocaInst>(Record.Anchor);
          Alloca && HeaderInfo.AllocaToBeRedirected.contains(Alloca))
        return HeaderInfo.SentryAnchor->getNextNode();
    }

    ReplaceRef Replacement =
        findLatestOtherReplaceRecord(Context, Record.Anchor, San);
    if (Replacement.Record)
      if (auto *I = dyn_cast<Instruction>(Replacement.Record->SanitizerValue))
        return I;

    return Record.Anchor;
  };

  // Replay sanitizer blocks.
  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    AddArchive &Archive = Entry.Archive.AddRecords;
    for (auto &[Anchor, Pair] : Archive.SanBBMap) {
      Function *F;
      BasicBlock *InsertBefore;
      auto &[AnchorType, BBs] = Pair;

      if (AnchorType == AddArchive::FunctionAnchor) {
        requireOrDie(Anchor, "resan add replay lost a function anchor");
        F = static_cast<Function *>(Anchor);
        InsertBefore = nullptr;
      } else if (AnchorType == AddArchive::BasicBlockAnchor) {
        requireOrDie(Anchor, "resan add replay lost a user block anchor");
        InsertBefore = static_cast<BasicBlock *>(Anchor);
        F = InsertBefore->getParent();
      } else {
        report_fatal_error("unreachable code in resan add replay anchor");
      }

      for (BasicBlock *BB : BBs) {
        requireOrDie(BB, "resan add replay lost a sanitizer block");
        requireOrDie(
            !BB->getParent(),
            "resan add replay found a sanitizer block already in a function");
        for (Instruction &SanInst : *BB)
          Entry.San.tag(SanInst);
        BB->insertInto(F, InsertBefore);
      }
    }
  }

  SmallVector<AddRecordRef, 64> Records;
  for (SanitizerArchive &Entry : Context.SanitizerArchives)
    for (AddRecord &Record : Entry.Archive.AddRecords.Records)
      Records.push_back({&Entry.San, &Record});

  // Replay add records.
  // We need to record how CFG changed after replay split type add record to
  // update PHI nodes.
  DenseMap<BasicBlock *, SmallVector<std::pair<BasicBlock *, BasicBlock *>>>
      PhiUpdateMap;
  SmallVector<PHINode *, 16> PhiToUpdate;
  BitVector Restored(Records.size());
  size_t Remaining = Records.size();
  while (Remaining != 0) {
    bool Progress = false;
    for (size_t I = 0; I < Records.size(); ++I) {
      if (Restored.test(I))
        continue;

      const Sanitizer &San = *Records[I].San;
      AddRecord &Add = *Records[I].Record;
      Instruction *Anchor = RedirectAnchor(San, Add);
      BasicBlock *UserBB = Anchor->getParent();
      requireOrDie(UserBB,
                   "resan add replay get anchor without a parent block");

      if (UserBB->getName() == AddRecord::MagicRecordBbName)
        continue;

      if (auto *Pure = std::get_if<AddRecord::PureRecord>(&Add.Record)) {
        for (Instruction &SanInst : *Pure->SanInsts) {
          San.tag(SanInst);
          if (auto *Phi = dyn_cast<PHINode>(&SanInst))
            PhiToUpdate.push_back(Phi);
        }
        __xsan::compat::spliceInstructions(UserBB, Anchor->getIterator(),
                                           Pure->SanInsts.get());
      } else if (auto *Split =
                     std::get_if<AddRecord::SplitRecord>(&Add.Record)) {
        for (Instruction &SanInst : *Split->SanInstsToAppend) {
          San.tag(SanInst);
          if (auto *Phi = dyn_cast<PHINode>(&SanInst))
            PhiToUpdate.push_back(Phi);
        }
        __xsan::compat::spliceInstructions(Split->LastBB, Split->LastBB->end(),
                                           UserBB, Anchor->getIterator(),
                                           UserBB->end());
        __xsan::compat::spliceInstructions(UserBB, UserBB->end(),
                                           Split->SanInstsToAppend.get());

        Split->LastBB->replaceSuccessorsPhiUsesWith(UserBB, Split->LastBB);
        for (BasicBlock *Succ : successors(Split->LastBB)) {
          auto &Vec = PhiUpdateMap[Succ];
          bool NeedInsert = true;
          for (auto &[OldBB, NewBB] : Vec) {
            if (OldBB == UserBB)
              NeedInsert = false;
            if (NewBB == UserBB)
              NewBB = Split->LastBB;
          }
          if (NeedInsert)
            Vec.push_back({UserBB, Split->LastBB});
        }
      } else {
        report_fatal_error("unreachable code in resan add replay record");
      }

      Restored.set(I);
      --Remaining;
      Progress = true;
    }
    requireOrDie(
        Progress,
        "resan add replay encountered a cycle in add anchor dependencies");
  }

  for (PHINode *Phi : PhiToUpdate) {
    if (auto It = PhiUpdateMap.find(Phi->getParent());
        It != PhiUpdateMap.end()) {
      for (auto &[OldBB, NewBB] : It->second)
        Phi->replaceIncomingBlockWith(OldBB, NewBB);
    }
  }

  // Sentry anchors are temporary ordering aids, not sanitizer records. Remove
  // them after add replay unless debug mode keeps IR artifacts visible.
  if (!options::ClDebug)
    for (Instruction *Sentry : SentryAnchors)
      if (Sentry->getParent())
        Sentry->eraseFromParent();
}

static void replayReplace(Module &M, SanitizingContext &Context) {
  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    for (auto &[UserValue, Records] : Entry.Archive.ReplaceRecords) {
      requireOrDie(Records.size() == 1,
                   Twine("resan found multiple replace records in sanitizer ") +
                       Entry.San.name());
      ReplaceRecord &Record = Records.front();
      Value *ResolvedValue =
          resolveReplayReplacement(Context, Entry.San, Record.SanitizerValue);
      if (!ResolvedValue)
        continue;
      replaceNonSanitizerUses(Entry.San, *UserValue, *ResolvedValue);
    }
  }
}

static bool shouldReplayDelete(const SanitizingContext &Context,
                               Instruction &UserInst) {
  ReplaceRef Replacement = findLatestReplaceRecord(Context, &UserInst, nullptr,
                                                   /*IncludeBase=*/true);
  if (!Replacement.Record)
    return true;

  Value *ResolvedValue = resolveReplayReplacement(
      Context, *Replacement.San, Replacement.Record->SanitizerValue);
  return ResolvedValue && ResolvedValue != &UserInst;
}

static void replayDelete(Module &M, SanitizingContext &Context) {
  for (SanitizerArchive &Entry : Context.SanitizerArchives) {
    for (auto &[UserInst, Records] : Entry.Archive.DeleteRecords) {
      if (!UserInst->getParent())
        continue;
      if (!shouldReplayDelete(Context, *UserInst))
        continue;
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

void runReSan(Module &M, SanitizingContext &Context) {
  for (const SanitizerArchive &Entry : Context.SanitizerArchives)
    validateReplaceArchive(Entry.San, Entry.Archive.ReplaceRecords);

  replayAdd(M, Context);
  replayReplace(M, Context);
  replayDelete(M, Context);
  replayFunction(M, Context);
}

ReSanPass::ReSanPass(std::shared_ptr<SanitizingContext> Ctx)
    : Ctx(std::move(Ctx)) {}

PreservedAnalyses ReSanPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!Ctx, "resan replay pass missing pipeline context");

  if (options::ClDebug)
    for (const SanitizerArchive &Entry : Ctx->SanitizerArchives)
      validateArchive(Entry.San, Entry.Archive, Ctx->UserBBs);

  runReSan(M, *Ctx);

  if (options::ClDebug)
    requireOrDie(!llvm::verifyModule(M, &llvm::errs()),
                 "resan replay produced invalid IR");

  return PreservedAnalyses::all();
}

} // namespace __xsan::desan
