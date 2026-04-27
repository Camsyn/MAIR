#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"
#include "Utils/UbsanUtils.h"
#include "XSanDeSanState.h"
#include "xsan_llvm_compat.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>

using namespace llvm;

namespace __xsan::desan {

static void restoreFunction(StageKind Stage, FunctionArchive &Archive,
                            Module &M, const UserBBsMap &UserBBsMap) {
  size_t BeginSize = Archive.size();

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (UserBBsMap.count(&F))
      continue;
    Archive.push_back({Stage, &F});
  }

  for (size_t I = BeginSize; I < Archive.size(); ++I) {
    Archive[I].Func->removeFromParent();
  }
}

static void restoreDelete(StageKind Stage, DeleteArchive &Archive,
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
                "stage ") +
              getStageName(Stage));
      requireOrDie(Event.DeletedValue->getNumOperands() ==
                       Event.Operands.size(),
                   Twine("desan delete restore found operand count mismatch in "
                         "stage ") +
                       getStageName(Stage));

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

      Archive[Event.DeletedValue].push_back({Stage});
      Restored.set(i);
      --Remaining;
      Progress = true;
    }
    requireOrDie(Progress,
                 Twine("desan could not resolve delete anchors in stage ") +
                     getStageName(Stage));
  }
}

static void restoreReplace(StageKind Stage, ReplaceArchive &Archive,
                           std::vector<ReplaceEvent> &Events) {
  for (const auto &Event : Events) {
    replaceUserIRUses(*Event.ReplaceValue, *Event.UserValue);
    Archive[Event.UserValue].push_back({Stage, Event.ReplaceValue});
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

static void restoreAdd(StageKind Stage, AddArchive &Archive, Module &M,
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
                         "for a split add in stage ") +
                       getStageName(Stage));
      requireOrDie(
          UserBB != BB,
          Twine("desan add restore found the split block itself as the "
                "user block anchor for a split add in stage ") +
              getStageName(Stage));

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
                "the user block anchor for a split add in stage ") +
              getStageName(Stage));

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
          {Stage, AddRecord::AddRecordKind::Split, UserI, std::move(Record)});

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
                           "instruction after a pure add in stage ") +
                         getStageName(Stage));

        AddRecord::PureRecord Record;
        Record.SanInsts.reset(
            BasicBlock::Create(BB.getContext(), AddRecord::MagicRecordBbName));
        __xsan::compat::spliceInstructions(Record.SanInsts.get(),
                                           Record.SanInsts->end(), &BB,
                                           SanItBegin, SanItEnd);
        Archive.Records.push_back({Stage, AddRecord::AddRecordKind::Pure,
                                   &*SanItEnd, std::move(Record)});
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
    return !UserI || !isStageIR(StageKind::UBSan, *UserI);
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

static void restoreUbsanArithmetic(ReSanArchive &Archive, Module &M) {
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
    replaceNonStageIRUses(StageKind::UBSan, *EI, *Restored);
    Archive.ReplaceRecords[Restored].push_back({StageKind::UBSan, EI});
    Archive.DeleteRecords[Restored].push_back({StageKind::UBSan});
  }
}

void runDeSan(Module &M, StageKind Stage, ReSanArchive &Archive,
              UserBBsMap &UserBBsMap) {
  if (Stage == StageKind::UBSan) {
    restoreUbsanArithmetic(Archive, M);
    return;
  }

  auto DeleteEvents = getDeleteEvents();
  auto ReplaceEvents = getReplaceEvents();

  restoreFunction(Stage, Archive.FunctionRecords, M, UserBBsMap);
  restoreDelete(Stage, Archive.DeleteRecords, DeleteEvents);
  restoreReplace(Stage, Archive.ReplaceRecords, ReplaceEvents);
  restoreAdd(Stage, Archive.AddRecords, M, UserBBsMap);

  if (options::ClDebug) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(
        (llvm::Twine(getStageName(Stage)) + ".archive.txt").str(), EC,
        llvm::sys::fs::OF_None);
    dumpArchive(Archive, OS);
  }
}

} // namespace __xsan::desan
