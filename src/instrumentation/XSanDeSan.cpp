#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"
#include "Utils/UbsanUtils.h"
#include "XSanDeSanReSan.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>

using namespace llvm;

namespace __xsan::desan {
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

  return PreservedAnalyses::none();
}

UserTaggingPass::UserTaggingPass(std::shared_ptr<SanitizingContext> Ctx)
    : Ctx(std::move(Ctx)) {}

PreservedAnalyses UserTaggingPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!Ctx, "desan user tagging pass missing pipeline context");

  markUserIR(M);
  Ctx->UserBBs = collectUserBBs(M);

  return PreservedAnalyses::none();
}

DeSanPass::DeSanPass(Sanitizer San, std::shared_ptr<SanitizingContext> Ctx)
    : San(San), Ctx(std::move(Ctx)) {}

PreservedAnalyses DeSanPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!Ctx, "desan restore pass missing pipeline context");

  Ctx->dumpNextIR(M, San.name());

  SanitizerArchive &Archive = Ctx->addArchive(San);
  runDeSan(M, Archive, Ctx->UserBBs);

  Ctx->dumpNextIR(M, "desan");

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
    const bool RealReplacement =
        replaceNonSanInstUses(San, *Event.SanReplacer, *Event.UserReplacee);
    Archive[Event.UserReplacee].push_back({Event.SanReplacer, RealReplacement});
  }
}

static bool isCurrentSanitizerInst(const Sanitizer &San, const Instruction &I) {
  if (San.isUBSan())
    return San.is(I);
  return !__xsan::UserIR::is(I);
}

static UserBBsMap collectUserBBs(const Sanitizer &San, Module &M) {
  UserBBsMap Result;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    const bool HasUserInst = llvm::any_of(instructions(F), [](Instruction &I) {
      return __xsan::UserIR::is(I) && !isa<DbgInfoIntrinsic>(I);
    });
    for (BasicBlock &BB : F) {
      if (pred_empty(&BB)) {
        if (HasUserInst)
          Result[&F].insert(&BB);
        continue;
      }
      if (llvm::any_of(predecessors(&BB), [&](BasicBlock *Pred) {
            Instruction *Term = Pred->getTerminator();
            return !Term || !isCurrentSanitizerInst(San, *Term);
          }))
        Result[&F].insert(&BB);
    }
  }
  return Result;
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
                       San.name() + " function " + F.getName() + " block " +
                       BB->getName());
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
                         San.name() + " function " + F.getName() + " block " +
                         BB.getName());

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

static bool allInstructionUsesAreUbsan(const Value &V) {
  for (const User *U : V.users()) {
    const auto *UserI = dyn_cast<Instruction>(U);
    if (!UserI || !__xsan::UBSanInst::is(*UserI))
      return false;
  }
  return true;
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
  if (!getUbsanReplaceRule(WOI->getIntrinsicID(), Rule))
    return nullptr;
  if (!hasTaggedUbsanOverflowExtract(*WOI))
    return nullptr;
  return WOI;
}

static Instruction *foldUBSanReplacement(
    Instruction &I, const DenseMap<Value *, Value *> &RestoredValues) {
  UbsanReplaceRule Rule;
  WithOverflowInst *WOI = matchUBSanReplacement(I, Rule);
  if (!WOI)
    return nullptr;

  auto &EI = cast<ExtractValueInst>(I);

  auto resolveOperand = [&](Value *V) -> Value * {
    auto It = RestoredValues.find(V);
    return It == RestoredValues.end() ? V : It->second;
  };
  Value *LHS = resolveOperand(WOI->getArgOperand(0));
  Value *RHS = resolveOperand(WOI->getArgOperand(1));
  Twine Name = EI.hasName() ? EI.getName() : "";

  auto *Restored = BinaryOperator::Create(Rule.Opcode, LHS, RHS, Name);
  if (Rule.HasNoSignedWrap)
    Restored->setHasNoSignedWrap();

  __xsan::compat::insertInstructionBefore(Restored, WOI);
  Restored->setDebugLoc(EI.getDebugLoc());
  __xsan::UserIR::set(*Restored);
  return Restored;
}

static void markUbsanReplaceOperandChainAsUser(Instruction &Root) {
  SmallVector<Instruction *, 8> Worklist;
  SmallPtrSet<Instruction *, 16> Visited;
  Worklist.push_back(&Root);

  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();
    if (!Visited.insert(I).second)
      continue;

    __xsan::UBSanInst::drop(*I);
    __xsan::UserIR::set(*I);

    for (Value *Operand : I->operands())
      if (auto *OperandI = dyn_cast<Instruction>(Operand))
        if (__xsan::UBSanInst::is(*OperandI))
          Worklist.push_back(OperandI);
  }
}

static void restoreUbsanArithmetic(const Sanitizer &San, ReSanArchive &Archive,
                                   Module &M) {
  SmallVector<Instruction *, 16> Worklist;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        UbsanReplaceRule Rule;
        if (matchUBSanReplacement(I, Rule))
          Worklist.push_back(&I);
      }
    }
  }

  DenseMap<Value *, Value *> RestoredValues;
  for (Instruction *I : Worklist) {
    UbsanReplaceRule Rule;
    auto *WOI = matchUBSanReplacement(*I, Rule);
    if (!WOI)
      continue;
    auto &EI = cast<ExtractValueInst>(*I);
    auto *Restored = foldUBSanReplacement(EI, RestoredValues);
    requireOrDie(Restored, "desan ubsan arithmetic restore lost its pattern");
    RestoredValues[&EI] = Restored;
    markUbsanReplaceOperandChainAsUser(*Restored);
    replaceNonSanitizerUses(San, EI, *Restored);
    replaceDbgUsesWithUndef(&EI);
    if (allInstructionUsesAreUbsan(EI)) {
      __xsan::UBSanInst::set(EI);
      __xsan::UserIR::drop(EI);
    }
    if (allInstructionUsesAreUbsan(*WOI)) {
      __xsan::UBSanInst::set(*WOI);
      __xsan::UserIR::drop(*WOI);
    }
    Archive.ReplaceRecords[Restored].push_back({&EI});
    Archive.DeleteRecords[Restored].push_back({});
  }
}

void runDeSan(Module &M, SanitizerArchive &Entry, UserBBsMap &UserBBsMap) {
  const Sanitizer &San = Entry.San;
  ReSanArchive &Archive = Entry.Archive;

  UserBBsMap = collectUserBBs(San, M);

  if (San.isUBSan()) {
    if (!__xsan::hasUbsanFunction(M))
      return;
    markUserIR(M);
    UserBBsMap = collectUserBBs(San, M);
    restoreUbsanArithmetic(San, Archive, M);
    UserBBsMap = collectUserBBs(San, M);
    restoreAdd(San, Archive.AddRecords, M, UserBBsMap);
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
