#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"
#include "Utils/UbsanUtils.h"
#include "XSanDeSanReSan.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>

using namespace llvm;

namespace __xsan::desan {
PreprocessPass::PreprocessPass(std::shared_ptr<SanitizingContext> Ctx)
    : Ctx(std::move(Ctx)) {}

PreservedAnalyses PreprocessPass::run(Module &M, ModuleAnalysisManager &MAM) {
  requireOrDie(!!Ctx, "desan preprocess pass missing pipeline context");

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
  Ctx->UserBBs = collectUserBBs(M);
  markUserIR(M);

  return PreservedAnalyses::all();
}

DeSanPass::DeSanPass(llvm::StringRef SanName,
                     std::shared_ptr<SanitizingContext> Ctx)
    : San(Sanitizer::Create(SanName)), Ctx(std::move(Ctx)) {}

PreservedAnalyses DeSanPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!Ctx, "desan restore pass missing pipeline context");

  SanitizerArchive &Archive = Ctx->addArchive(San);
  runDeSan(M, Archive, Ctx->UserBBs);

  return PreservedAnalyses::all();
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

static void restoreDelete(const Sanitizer &San, DeleteArchive &Archive,
                          std::vector<DeleteEvent> &Events) {

  BitVector Restored(Events.size());
  size_t Remaining = Events.size();
  while (Remaining != 0) {
    bool Progress = false;
    for (size_t i = 0; i < Events.size(); ++i) {
      if (Restored.test(i))
        continue;

      auto &Event = Events[i];
      requireOrDie(
          Event.DeletedValue && !Event.DeletedValue->getParent(),
          Twine("desan delete restore found a non-orphan deleted value in "
                "sanitizer ") +
              San.name());
      requireOrDie(Event.DeletedValue->getNumOperands() ==
                       Event.Operands.size(),
                   Twine("desan delete restore found operand count mismatch in "
                         "sanitizer ") +
                       San.name());

      for (size_t j = 0; j < Event.Operands.size(); ++j) {
        llvm::Value *Operand = Event.Operands[j];

        // invoke instructions' BB operands may be replaced with unreachable
        // instructions (removeUnreachableBlocks), which are not marked as user
        // IR.
        requireOrDie(
            !llvm::isa<llvm::Instruction>(Operand) ||
                llvm::isa<llvm::UnreachableInst>(Operand) ||
                __xsan::UserIR::is(*llvm::cast<llvm::Instruction>(Operand)),
            "desan delete restore found a non-user operand");

        Event.DeletedValue->setOperand(j, Operand);
      }

      if (std::holds_alternative<BasicBlock *>(Event.Anchor)) {
        BasicBlock *BB = std::get<BasicBlock *>(Event.Anchor);
        __xsan::compat::appendInstruction(BB, Event.DeletedValue);
      } else {
        Instruction *I = std::get<Instruction *>(Event.Anchor);
        if (!I->getParent())
          continue;
        __xsan::compat::insertInstructionBefore(Event.DeletedValue, I);
      }

      Archive[Event.DeletedValue].push_back({});
      Restored.set(i);
      --Remaining;
      Progress = true;
    }
    requireOrDie(Progress,
                 Twine("desan could not resolve delete anchors in sanitizer ") +
                     San.name());
  }
}

static void restoreReplace(const Sanitizer &San, ReplaceArchive &Archive,
                           std::vector<ReplaceEvent> &Events) {
  for (const auto &Event : Events) {
    replaceUserIRUses(*Event.ReplaceValue, *Event.UserValue);
    Archive[Event.UserValue].push_back({Event.ReplaceValue});
  }
}

static BasicBlock *
findUserBBForSplit(BasicBlock *SplitBB, const UserBBsSet &UserBBs,
                   const llvm::SmallPtrSetImpl<BasicBlock *> &HasUserInsts) {
  llvm::SmallVector<BasicBlock *, 8> Worklist;
  llvm::SmallPtrSet<BasicBlock *, 16> Visited;

  for (BasicBlock *Pred : predecessors(SplitBB)) {
    if (Pred == SplitBB)
      continue;
    if (Visited.insert(Pred).second)
      Worklist.push_back(Pred);
  }

  while (!Worklist.empty()) {
    BasicBlock *Cur = Worklist.pop_back_val();
    if (UserBBs.contains(Cur) || HasUserInsts.contains(Cur))
      return Cur;

    for (BasicBlock *Pred : predecessors(Cur)) {
      if (Pred == Cur || Pred == SplitBB)
        continue;
      if (Visited.insert(Pred).second)
        Worklist.push_back(Pred);
    }
  }

  return nullptr;
}

static void restoreAdd(const Sanitizer &San, AddArchive &Archive, Module &M,
                       const UserBBsMap &UserBBsMap) {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    const auto UserBBsIt = UserBBsMap.find(&F);
    if (UserBBsIt == UserBBsMap.end())
      continue;
    const UserBBsSet &UserBBs = UserBBsIt->second;

    // ------ Collect split type add blocks ------

    // Collect sanitizer add blocks.
    llvm::SmallVector<std::pair<BasicBlock *, Instruction *>> FirstUserInsts;
    llvm::SmallPtrSet<BasicBlock *, 8> HasUserInsts;
    llvm::SmallVector<BasicBlock *, 8> SanitizerBBs;

    llvm::Function::iterator SanBBsBegin, SanBBsEnd;
    SanBBsEnd = F.begin();
    while (SanBBsEnd != F.end()) {
      SanBBsBegin = std::find_if(SanBBsEnd, F.end(), [&](BasicBlock &BB) {
        return !UserBBs.contains(&BB);
      });
      if (SanBBsBegin == F.end())
        break;
      SanBBsEnd = std::find_if(SanBBsBegin, F.end(), [&](BasicBlock &BB) {
        return UserBBs.contains(&BB);
      });

      AddArchive::BBAnchor Anchor;
      AddArchive::BBAnchorType AnchorType;
      if (SanBBsEnd == F.end()) {
        Anchor = &F;
        AnchorType = AddArchive::BBAnchorType::FunctionAnchor;
      } else {
        Anchor = &*SanBBsEnd;
        AnchorType = AddArchive::BBAnchorType::BasicBlockAnchor;
      }

      auto [It, Inserted] = Archive.SanBBMap.insert({Anchor, {AnchorType, {}}});
      if (Inserted)
        It->second.first = AnchorType;
      for (BasicBlock &BB : llvm::make_range(SanBBsBegin, SanBBsEnd)) {
        It->second.second.push_back(&BB);
        SanitizerBBs.push_back(&BB);
        // Sanitizer blocks with user instructions mean user block are split.
        // Check and collect for the later restoration.
        if (auto It = llvm::find_if(
                BB, [&](Instruction &I) { return __xsan::UserIR::is(I); });
            It != BB.end()) {
          FirstUserInsts.push_back({&BB, &*It});
          HasUserInsts.insert(&BB);
        }
      }
    }

    // Collect split type add blocks
    for (auto [BB, UserI] : FirstUserInsts) {
      // Find the nearest user/split anchor block before this split block.
      auto *UserBB = findUserBBForSplit(BB, UserBBs, HasUserInsts);
      requireOrDie(UserBB != nullptr,
                   Twine("desan add restore could not find a user block anchor "
                         "for a split add in sanitizer ") +
                       San.name());
      requireOrDie(
          UserBB != BB,
          Twine("desan add restore found the split block itself as the "
                "user block anchor for a split add in sanitizer ") +
              San.name());

      // Find the first sanitizer instruction after the last user instruction in
      // the user block.
      Instruction *FirstSanI;
      if (auto It = llvm::find_if(
              llvm::reverse(*UserBB),
              [&](Instruction &I) { return __xsan::UserIR::is(I); });
          It != UserBB->rend()) {
        FirstSanI = It->getNextNode();
      } else {
        FirstSanI = &UserBB->front();
      }
      requireOrDie(
          FirstSanI,
          Twine("desan add restore could not find a sanitizer instruction in "
                "the user block anchor for a split add in sanitizer ") +
              San.name());

      // Store the rest sanitizer instructions in the user block and
      // restore all instructions in the last sanitizer block.
      AddRecord::SplitRecord Record;
      Record.SanInstsToAppend.reset(
          BasicBlock::Create(F.getContext(), AddRecord::MagicRecordBbName));
      __xsan::compat::spliceInstructions(
          Record.SanInstsToAppend.get(), Record.SanInstsToAppend->end(), UserBB,
          FirstSanI->getIterator(), UserBB->end());

      Record.LastBB = BB;
      __xsan::compat::spliceInstructions(UserBB, UserBB->end(), BB,
                                         UserI->getIterator(), BB->end());
      UserBB->replaceSuccessorsPhiUsesWith(BB, UserBB);

      Archive.Records.push_back(
          {AddRecord::AddRecordKind::Split, UserI, std::move(Record)});

      HasUserInsts.erase(BB);
    }

    // Remove the collected sanitizer add blocks without user instructions.
    for (BasicBlock *BB : SanitizerBBs)
      BB->removeFromParent();

    // ------ Collect pure type add blocks ------

    for (BasicBlock &BB : F) {
      if (!UserBBs.contains(&BB))
        continue;

      BasicBlock::iterator SanItBegin, SanItEnd;
      SanItBegin = llvm::find_if(
          BB, [&](Instruction &I) { return !__xsan::UserIR::is(I); });
      while (SanItBegin != BB.end()) {
        SanItEnd = std::find_if(SanItBegin, BB.end(), [&](Instruction &I) {
          return __xsan::UserIR::is(I);
        });
        requireOrDie(SanItEnd != BB.end(),
                     Twine("desan add restore could not find a user "
                           "instruction after a pure add in sanitizer ") +
                         San.name());

        AddRecord::PureRecord Record;
        Record.SanInsts.reset(
            BasicBlock::Create(BB.getContext(), AddRecord::MagicRecordBbName));
        __xsan::compat::spliceInstructions(Record.SanInsts.get(),
                                           Record.SanInsts->end(), &BB,
                                           SanItBegin, SanItEnd);
        Archive.Records.push_back(
            {AddRecord::AddRecordKind::Pure, &*SanItEnd, std::move(Record)});
        SanItBegin = std::find_if(SanItEnd, BB.end(), [&](Instruction &I) {
          return !__xsan::UserIR::is(I);
        });
      }
    }
  }
}

static bool isUbsanArithmeticIntrinsic(const IntrinsicInst &II) {
  switch (II.getIntrinsicID()) {
  case Intrinsic::sadd_with_overflow:
  case Intrinsic::uadd_with_overflow:
  case Intrinsic::ssub_with_overflow:
  case Intrinsic::usub_with_overflow:
  case Intrinsic::smul_with_overflow:
  case Intrinsic::umul_with_overflow:
    return __xsan::UBSanInst::is(II);
  default:
    return false;
  }
}

static bool hasNonUbsanUses(const Value &V) {
  return llvm::any_of(V.users(), [](const User *U) {
    const auto *UserI = dyn_cast<Instruction>(U);
    return !UserI || !__xsan::UBSanInst::is(*UserI);
  });
}

static Instruction *createRestoredUbsanArithmetic(ExtractValueInst &EI,
                                                  IntrinsicInst &II) {
  Value *LHS = II.getArgOperand(0);
  Value *RHS = II.getArgOperand(1);
  Instruction *Restored = nullptr;
  Twine Name = EI.hasName() ? EI.getName() : "";

  switch (II.getIntrinsicID()) {
  case Intrinsic::sadd_with_overflow:
  case Intrinsic::uadd_with_overflow:
    Restored = BinaryOperator::Create(Instruction::Add, LHS, RHS, Name);
    break;
  case Intrinsic::ssub_with_overflow:
  case Intrinsic::usub_with_overflow:
    Restored = BinaryOperator::Create(Instruction::Sub, LHS, RHS, Name);
    break;
  case Intrinsic::smul_with_overflow:
  case Intrinsic::umul_with_overflow:
    Restored = BinaryOperator::Create(Instruction::Mul, LHS, RHS, Name);
    break;
  default:
    llvm_unreachable("unexpected UBSan arithmetic intrinsic");
  }

  __xsan::compat::insertInstructionBefore(Restored, &EI);
  Restored->setDebugLoc(EI.getDebugLoc());
  __xsan::UserIR::set(*Restored);
  return Restored;
}

static void restoreUbsanArithmetic(const Sanitizer &San, ReSanArchive &Archive,
                                   Module &M) {
  SmallVector<ExtractValueInst *, 16> Worklist;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *EI = dyn_cast<ExtractValueInst>(&I);
        if (!EI || EI->getNumIndices() != 1 || EI->getIndices()[0] != 0)
          continue;

        auto *II = dyn_cast<IntrinsicInst>(EI->getAggregateOperand());
        if (!II || !isUbsanArithmeticIntrinsic(*II) || !hasNonUbsanUses(*EI))
          continue;

        Worklist.push_back(EI);
      }
    }
  }

  for (ExtractValueInst *EI : Worklist) {
    auto *II = dyn_cast<IntrinsicInst>(EI->getAggregateOperand());
    auto *Restored = createRestoredUbsanArithmetic(*EI, *II);
    replaceNonSanitizerUses(San, *EI, *Restored);
    Archive.ReplaceRecords[Restored].push_back({EI});
    Archive.DeleteRecords[Restored].push_back({});
  }
}

void runDeSan(Module &M, SanitizerArchive &Entry, UserBBsMap &UserBBsMap) {
  const Sanitizer &San = Entry.San;
  ReSanArchive &Archive = Entry.Archive;

  if (San.isUBSan()) {
    restoreUbsanArithmetic(San, Archive, M);
    return;
  }

  auto DeleteEvents = getDeleteEvents();
  auto ReplaceEvents = getReplaceEvents();

  restoreFunction(San, Archive.FunctionRecords, M, UserBBsMap);
  restoreDelete(San, Archive.DeleteRecords, DeleteEvents);
  restoreReplace(San, Archive.ReplaceRecords, ReplaceEvents);
  restoreAdd(San, Archive.AddRecords, M, UserBBsMap);

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS((llvm::Twine(San.name()) + ".archive.txt").str(),
                            EC, llvm::sys::fs::OF_None);
    dumpArchive(San, Archive, OS);
  }
}

} // namespace __xsan::desan
