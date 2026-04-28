#include "Utils/Options.h"
#include "XSanDeSanState.h"
#include "xsan_llvm_compat.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace __xsan::desan {

namespace {

static bool isLaterStage(StageKind LHS, StageKind RHS) {
  return static_cast<uint32_t>(LHS) > static_cast<uint32_t>(RHS);
}

static Value *resolveReplayReplacement(StageKind Stage, Value *SanitizerValue,
                                       const ReplaceArchive &ReplaceArchive,
                                       const DeleteArchive &DeleteArchive) {
  if (!SanitizerValue)
    return nullptr;

  SmallPtrSet<Value *, 8> Visited;
  Value *CurrentValue = SanitizerValue;
  StageKind CurrentStage = Stage;

  while (Visited.insert(CurrentValue).second) {
    bool Advanced = false;
    if (auto It = ReplaceArchive.find(CurrentValue);
        It != ReplaceArchive.end()) {
      const auto &Next = It->second.front();
      if (isLaterStage(Next.Stage, CurrentStage) && Next.SanitizerValue) {
        CurrentValue = Next.SanitizerValue;
        CurrentStage = Next.Stage;
        Advanced = true;
      }
    }
    if (Advanced)
      continue;

    if (auto *I = dyn_cast<Instruction>(CurrentValue)) {
      if (auto It = DeleteArchive.find(I); It != DeleteArchive.end()) {
        if (llvm::any_of(It->second, [&](const DeleteRecord &Record) {
              return isLaterStage(Record.Stage, CurrentStage);
            }))
          return nullptr;
      }
    }
    return CurrentValue;
  }

  return nullptr;
}

} // namespace

// We need ReplaceArchive to redirect instruction anchor, and DeleteArchive to
// add the sentry anchor into it.
static void replayAdd(Module &M, AddArchive &Archive,
                      DeleteArchive &DeleteArchive,
                      const ReplaceArchive &ReplaceArchive) {
  // Some sanitizer replace arguments with their sanitized versions, but some
  // sanitizer assume the arguments are prepared at the the beginning of the
  // function, so we need to restrict the eariest anchor to be before the first
  // non-alloca instruction in the entry block, and we also need to add a sentry
  // anchor ensure the replacement can be earier than other non-argument-replace
  // instructions.
  struct FunctionHeaderInfo {
    SmallPtrSet<AllocaInst *, 8> AllocaToBeRedirected;
    Instruction *SentryAnchor;
    // We simplify the logic checking whether an add record contains argument
    // replacement by recording all basic blocks containing these replacements.
    // Should only contain BBs in add records.
    SmallPtrSet<BasicBlock *, 8> BBsContainingReplace;
  };
  DenseMap<Function *, FunctionHeaderInfo> FunctionHeaderMap;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;

    // Collect instructions replacing arguments in this function
    SmallVector<Instruction *, 8> ReplaceInsts;
    for (auto &Arg : F.args()) {
      if (auto It = ReplaceArchive.find(&Arg); It != ReplaceArchive.end()) {
        const auto &Record = It->second.front();
        if (auto *Inst = dyn_cast<Instruction>(Record.SanitizerValue))
          ReplaceInsts.push_back(Inst);
      }
    }
    if (ReplaceInsts.empty())
      continue;

    auto &EntryBB = F.getEntryBlock();
    auto &HeaderInfo = FunctionHeaderMap[&F];

    // Collect basic blocks containing these replacements
    for (Instruction *Inst : ReplaceInsts)
      HeaderInfo.BBsContainingReplace.insert(Inst->getParent());

    // Build the sentry anchor and collect the alloca to be redirected
    auto It = llvm::find_if(
        EntryBB, [](const Instruction &I) { return !isa<AllocaInst>(I); });
    requireOrDie(It != EntryBB.end(), "resan add replay found a BB without a "
                                      "non-alloca terminator instruction");
    auto *DoNothingFunc =
        __xsan::compat::getOrInsertDeclaration(&M, Intrinsic::donothing);
    HeaderInfo.SentryAnchor = CallInst::Create(DoNothingFunc);
    __xsan::compat::insertInstructionBefore(HeaderInfo.SentryAnchor, &*It);
    for (Instruction &I : llvm::make_range(
             EntryBB.begin(), HeaderInfo.SentryAnchor->getIterator()))
      HeaderInfo.AllocaToBeRedirected.insert(cast<AllocaInst>(&I));

    // Add the sentry anchor to delete record so that it will be removed
    // by delete replay.
    if (!options::ClDebug)
      DeleteArchive[HeaderInfo.SentryAnchor].push_back({StageKind::ReSan});
  }

  // Some add records' anchors may need to be redirected.
  // 1. If the sanitizer parts contains a instruction replacing the argument, we
  //    should redirect to the header sentry mentioned above.
  // 2. If the anchor is an alloca instruction at the function header, we should
  //    redirect to the instruction after the header sentry mentioned above.
  // 3. If anchor is replaced by another sanitizer, we should redirect to the
  //    new one. Because the add anchor means the add record should be before
  //    the anchor.
  const auto RedirectAnchor = [&ReplaceArchive, &FunctionHeaderMap](
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

    if (auto It = ReplaceArchive.find(Record.Anchor);
        It != ReplaceArchive.end()) {
      const auto &ReplaceRecord = It->second.front();
      if (ReplaceRecord.Stage != Record.Stage)
        if (auto *I = dyn_cast<Instruction>(ReplaceRecord.SanitizerValue))
          return I;
    }

    return Record.Anchor;
  };

  // Replay sanitizer blocks
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
      BB->insertInto(F, InsertBefore);
    }
  }

  // Replay add records.
  // We need to record how CFG changed after replay split type add record to
  // update PHI nodes.
  DenseMap<BasicBlock *, SmallVector<std::pair<BasicBlock *, BasicBlock *>>>
      PhiUpdateMap;
  SmallVector<PHINode *, 16> PhiToUpdate;
  BitVector Restored(Archive.Records.size());
  size_t Remaining = Archive.Records.size();
  while (Remaining != 0) {
    bool Progress = false;
    for (size_t i = 0; i < Archive.Records.size(); ++i) {
      if (Restored.test(i))
        continue;

      auto &[Stage, Kind, _, Record] = Archive.Records[i];
      Instruction *Anchor = RedirectAnchor(Archive.Records[i]);
      BasicBlock *UserBB = Anchor->getParent();
      requireOrDie(UserBB,
                   "resan add replay get anchor without a parent block");

      if (UserBB->getName() == AddRecord::MagicRecordBbName)
        continue;

      if (auto *Pure = std::get_if<AddRecord::PureRecord>(&Record)) {
        for (Instruction &SanInst : *Pure->SanInsts) {
          setStageIR(Stage, SanInst);
          if (auto *Phi = dyn_cast<PHINode>(&SanInst))
            PhiToUpdate.push_back(Phi);
        }
        __xsan::compat::spliceInstructions(UserBB, Anchor->getIterator(),
                                           Pure->SanInsts.get());
      } else if (auto *Split = std::get_if<AddRecord::SplitRecord>(&Record)) {
        for (Instruction &SanInst : *Split->SanInstsToAppend) {
          setStageIR(Stage, SanInst);
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

      Restored.set(i);
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
}

static void replayReplace(Module &M, ReplaceArchive &Archive,
                          const DeleteArchive &DeleteArchive) {
  for (auto &[UserValue, Records] : Archive) {
    auto &Record = Records.front();
    Value *ResolvedValue = resolveReplayReplacement(
        Record.Stage, Record.SanitizerValue, Archive, DeleteArchive);
    if (!ResolvedValue)
      continue;
    replaceNonStageIRUses(Record.Stage, *UserValue, *ResolvedValue);
  }
}

static bool shouldReplayDelete(Instruction &UserInst,
                               const ReplaceArchive &ReplaceArchive,
                               const DeleteArchive &DeleteArchive) {
  auto It = ReplaceArchive.find(&UserInst);
  if (It == ReplaceArchive.end())
    return true;

  const auto &Record = It->second.front();
  Value *ResolvedValue = resolveReplayReplacement(
      Record.Stage, Record.SanitizerValue, ReplaceArchive, DeleteArchive);
  return ResolvedValue && ResolvedValue != &UserInst;
}

static void replayDelete(Module &M, DeleteArchive &Archive,
                         const ReplaceArchive &ReplaceArchive) {
  for (auto &[UserInst, Records] : Archive) {
    if (!shouldReplayDelete(*UserInst, ReplaceArchive, Archive))
      continue;
    UserInst->eraseFromParent();
  }
}

static void replayFunction(Module &M, FunctionArchive &Archive) {
  for (auto &[Stage, F] : Archive) {
    M.getFunctionList().push_back(F);
  }
}

// Both replayAdd and replayReplace use the ReSanArchive.
// So we do the validation before both of them.
static void validateReplaceArchive(const ReplaceArchive &Archive) {
  for (auto &[UserValue, Records] : Archive) {
    if (Records.size() > 1) {
      std::string S;
      llvm::raw_string_ostream OS(S);
      UserValue->print(OS);
      OS << "\nEach record belongs to stage:\n";
      for (const ReplaceRecord &Record : Records)
        OS << "  " << getStageName(Record.Stage) << "\n";
      report_fatal_error(OS.str().c_str());
    }
  }
}

void runReSan(Module &M, ReSanArchive &Archive) {
  validateReplaceArchive(Archive.ReplaceRecords);

  replayAdd(M, Archive.AddRecords, Archive.DeleteRecords,
            Archive.ReplaceRecords);
  replayReplace(M, Archive.ReplaceRecords, Archive.DeleteRecords);
  replayDelete(M, Archive.DeleteRecords, Archive.ReplaceRecords);

  replayFunction(M, Archive.FunctionRecords);
}

} // namespace __xsan::desan
