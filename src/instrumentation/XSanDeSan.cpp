#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"
#include "Utils/UbsanUtils.h"
#include "XSanDeSanReSan.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PatternMatch.h"
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

  return PreservedAnalyses::none();
}

UserTaggingPass::UserTaggingPass(std::shared_ptr<SanitizingContext> Ctx)
    : Ctx(std::move(Ctx)) {}

PreservedAnalyses UserTaggingPass::run(Module &M, ModuleAnalysisManager &) {
  requireOrDie(!!Ctx, "desan user tagging pass missing pipeline context");
  requireOrDie(Ctx->UBSanDeSanCompleted,
               "user tagging must run after UBSan DeSan");

  markUserIR(M);
  Ctx->UserBBs = collectUserBBs(M);

  return PreservedAnalyses::none();
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
  } while (Changed) ;
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

static UserBBsMap collectUserBBsForUBSan(Module &M) {
  UserBBsMap Result;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto &UserBBsSet = Result[&F];
    // 1. Rule ONE: Function entry must be user BB.
    UserBBsSet.insert(&F.getEntryBlock());
    // Traverse the function on topological order.
    ReversePostOrderTraversal RPO(&F);
    auto TermNonUBSan = [](const BasicBlock *BB) {
      assert(BB->getTerminator() && "Pathological BB without Terminator");
      return !UBSanInst::is(*BB->getTerminator());
    };
    for (auto *BB : llvm::make_range(RPO.begin(), RPO.end())) {
      if (UserBBsSet.contains(BB))
        continue;
      // 2. Rule TWO: BBs with pred_term_non_ubsan be user BBs.
      if (any_of(predecessors(BB), TermNonUBSan)) {
        UserBBsSet.insert(BB);
        continue;
      }
      // 3. Rule THREE: BB with 1) user inst and 2) >1 user entries
      //    The SinkBB is the only SanBB allowing user inst, but it must has
      //    just ONE entry.
      if (all_of(*BB, [](const Instruction &I) { return UBSanInst::is(I); }))
        continue;
      uint8_t NumUserEntry = 0;
      auto It = idf_begin(BB);
      ++It;
      for (auto End = idf_end(BB); It != End;) {
        BasicBlock *BB = *It;
        if (UserBBsSet.contains(BB)) {
          // >1 user entries, dirtecly break the loop.
          if (++NumUserEntry > 1)
            break;
          It.skipChildren();
        } else {
          ++It;
        }
      }
      if (NumUserEntry > 1)
        UserBBsSet.insert(BB);
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

  auto *Replacee = BinaryOperator::Create(
      Rule.Opcode, WithOverflow.getArgOperand(0), WithOverflow.getArgOperand(1),
      Name);
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
  restoreAdd(San, Archive.AddRecords, M, UserBBsMap);

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS((llvm::Twine(San.name()) + ".archive.txt").str(),
                            EC, llvm::sys::fs::OF_None);
    dumpArchive(San, Archive, OS);
  }
}

} // namespace __xsan::desan
