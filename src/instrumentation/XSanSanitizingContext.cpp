#include "XSanSanitizingContext.h"
#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>

using namespace llvm;

namespace __xsan::desan {

namespace {

template <typename T> static std::uintptr_t pointerOrderKey(T *Ptr) {
  return reinterpret_cast<std::uintptr_t>(Ptr);
}

static const char *getAddRecordKindName(AddRecord::AddRecordKind Kind) {
  switch (Kind) {
  case AddRecord::AddRecordKind::Pure:
    return "Pure";
  case AddRecord::AddRecordKind::Split:
    return "Split";
  case AddRecord::AddRecordKind::Redirect:
    return "Redirect";
  }
  return "<unknown>";
}

static void dumpFunctionRef(const Function *F, raw_ostream &OS) {
  if (!F) {
    OS << "<null>";
    return;
  }

  OS << "ptr=" << static_cast<const void *>(F) << " name=";
  if (F->hasName())
    OS << F->getName();
  else
    OS << "<unnamed>";
  OS << " decl=" << (F->isDeclaration() ? "yes" : "no");
}

static void dumpBasicBlockRef(const BasicBlock *BB, raw_ostream &OS) {
  if (!BB) {
    OS << "<null>";
    return;
  }

  OS << "ptr=" << static_cast<const void *>(BB) << " name=";
  if (BB->hasName())
    OS << BB->getName();
  else
    OS << "<unnamed>";

  if (const Function *F = BB->getParent()) {
    OS << " func=";
    if (F->hasName())
      OS << F->getName();
    else
      OS << "<unnamed>";
  } else {
    OS << " func=<detached>";
  }

  OS << " insts=" << BB->size();
}

static void dumpValueRef(const Value *V, raw_ostream &OS) {
  if (!V) {
    OS << "<null>";
    return;
  }

  OS << "ptr=" << static_cast<const void *>(V);

  if (const auto *I = dyn_cast<Instruction>(V)) {
    OS << " <inst opcode=" << I->getOpcodeName();
    if (I->hasName())
      OS << " name=" << I->getName();
    if (!I->getType()->isVoidTy()) {
      OS << " type=";
      I->getType()->print(OS);
    }
    OS << " operands=" << I->getNumOperands() << ">";
    OS << " ; user=" << (__xsan::UserIR::is(*I) ? "yes" : "no");
    if (const BasicBlock *BB = I->getParent()) {
      OS << " ; bb=";
      if (BB->hasName())
        OS << BB->getName();
      else
        OS << "<unnamed>";

      if (const Function *F = BB->getParent()) {
        OS << " ; func=";
        if (F->hasName())
          OS << F->getName();
        else
          OS << "<unnamed>";
      }
    } else {
      OS << " ; bb=<detached>";
    }
  } else if (const auto *BB = dyn_cast<BasicBlock>(V)) {
    OS << " <bb name=";
    if (BB->hasName())
      OS << BB->getName();
    else
      OS << "<unnamed>";
    OS << ">";
    if (const Function *F = BB->getParent()) {
      OS << " ; func=";
      if (F->hasName())
        OS << F->getName();
      else
        OS << "<unnamed>";
    } else {
      OS << " ; func=<detached>";
    }
    OS << " ; insts=" << BB->size();
  } else if (const auto *Arg = dyn_cast<Argument>(V)) {
    OS << " <arg no=" << Arg->getArgNo();
    if (Arg->hasName())
      OS << " name=" << Arg->getName();
    OS << " type=";
    Arg->getType()->print(OS);
    OS << ">";
    OS << " ; arg_no=" << Arg->getArgNo();
    if (const Function *F = Arg->getParent()) {
      OS << " ; func=";
      if (F->hasName())
        OS << F->getName();
      else
        OS << "<unnamed>";
    }
  } else if (const auto *F = dyn_cast<Function>(V)) {
    OS << " <function name=";
    if (F->hasName())
      OS << F->getName();
    else
      OS << "<unnamed>";
    OS << " decl=" << (F->isDeclaration() ? "yes" : "no") << ">";
  } else if (const auto *GV = dyn_cast<GlobalValue>(V)) {
    OS << " <global name=";
    if (GV->hasName())
      OS << GV->getName();
    else
      OS << "<unnamed>";
    OS << ">";
  } else if (const auto *C = dyn_cast<Constant>(V)) {
    OS << " <constant type=";
    C->getType()->print(OS);
    OS << ">";
  } else {
    OS << " <value kind=" << V->getValueID();
    if (V->hasName())
      OS << " name=" << V->getName();
    OS << ">";
  }
}

static void dumpInstList(const BasicBlock &BB, raw_ostream &OS,
                         const char *Indent) {
  size_t Index = 0;
  for (const Instruction &I : BB) {
    OS << Indent << "[" << Index++ << "] ";
    dumpValueRef(&I, OS);
    OS << "\n";
  }
}

static void dumpBasicBlockBody(const BasicBlock *BB, raw_ostream &OS,
                               const char *Indent) {
  if (!BB)
    return;

  size_t Index = 0;
  for (const Instruction &I : *BB) {
    OS << Indent << "[" << Index++ << "] ";
    dumpValueRef(&I, OS);
    OS << "\n";
  }
}

} // namespace

void requireOrDie(bool Condition, const Twine &Message) {
  if (!Condition)
    report_fatal_error(Message, false);
}

void SanitizingContext::clearArchives() {
  UserBBs.clear();
  UBSanDeSanCompleted = false;
  SanitizerArchives.clear();
}

SanitizerArchive &SanitizingContext::addArchive(const Sanitizer &San) {
  if (any_of(SanitizerArchives, [&San](const SanitizerArchive &Enrty) {
        return Enrty.San.sameAs(San);
      }))
    report_fatal_error(Twine("duplicate ReSanArchive for sanitizer ") +
                       San.name());

  return SanitizerArchives.emplace_back(*this, San);
}

void AddArchive::tagAs(const Sanitizer &San) {
  if (San.isUBSan())
    return;
  auto TagBB = [&San](BasicBlock &BB) {
    for (auto &I : BB)
      San.tag(I);
  };
  for (AddRecord &Record : Records)
    TagBB(*Record.SanInsts);

  for (PHIAddRecord &Record : PhiRecords)
    for (PHINode *Phi : Record.SanPhis)
      San.tag(*Phi);

  for (BlockAddRecord &BBRecord : BBRecords)
    for (auto [SanBB, _] : BBRecord.SanBBs)
      TagBB(*SanBB);
}

void AddArchive::addRecord(AddRecord &&Add) {
  Records.emplace_back(std::move(Add));
}

static void dumpIRToFile(Module &M, const Twine &FileName) {
  if (!options::enableDeSanReSanDump())
    return;

  SmallString<128> Dir(options::ClDumpDeSanReSan);
  if (std::error_code EC = sys::fs::create_directories(Dir))
    report_fatal_error(Twine("failed to create DeSan/ReSan dump directory '") +
                       Dir + "': " + EC.message());

  SmallString<256> Path(Dir);
  sys::path::append(Path, FileName.str());

  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_None);
  if (EC)
    report_fatal_error(Twine("failed to open DeSan/ReSan dump file '") + Path +
                       "': " + EC.message());
  M.print(OS, nullptr);
}

unsigned SanitizingContext::reserveDumpIndex() {
  if (!options::enableDeSanReSanDump())
    return NoDumpIndex;
  return NextDumpIndex++;
}

void SanitizingContext::dumpIR(Module &M, unsigned Index,
                               const Twine &Name) const {
  if (!options::enableDeSanReSanDump() || Index == NoDumpIndex)
    return;
  dumpIRToFile(M, formatv("{0:02}-{1}.ll", Index, Name).str());
}

unsigned SanitizingContext::dumpNextIR(Module &M, const Twine &Name) {
  unsigned Index = reserveDumpIndex();
  dumpIR(M, Index, Name);
  return Index;
}

void SanitizingContext::dumpNestedIR(Module &M, unsigned ParentIndex,
                                     const Twine &Name) const {
  dumpIR(M, ParentIndex, Name);
}

void markUserIR(Module &M) {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (auto &I : instructions(F))
      __xsan::UserIR::set(I);
  }
}

UserBBsMap collectUserBBs(llvm::Module &M) {
  UserBBsMap UserBBsMap;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    UserBBsSet &UserBBs = UserBBsMap[&F];
    for (BasicBlock &BB : F)
      UserBBs.insert(&BB);
  }
  return UserBBsMap;
}

bool replaceUserIRUses(Value &From, Value &To) {
  bool Changed = false;
  From.replaceUsesWithIf(&To, [&](Use &U) -> bool {
    auto *UserI = dyn_cast<Instruction>(U.getUser());
    const bool IsUserUse = UserI && __xsan::UserIR::is(*UserI);
    Changed |= IsUserUse;
    return IsUserUse;
  });
  return Changed;
}

bool replaceNonSanitizerUses(const Sanitizer &San, Value &From, Value &To) {
  bool Changed = false;
  From.replaceUsesWithIf(&To, [&](Use &U) -> bool {
    auto *UserI = dyn_cast<Instruction>(U.getUser());
    const bool IsSanitizerUse = UserI && San.is(*UserI);
    Changed |= !IsSanitizerUse;
    return !IsSanitizerUse;
  });
  return Changed;
}

void dumpArchive(const Sanitizer &San, const ReSanArchive &Archive,
                 llvm::raw_ostream &OS) {
  OS << "[ReSanArchive] sanitizer=" << San.name()
     << " add_records=" << Archive.AddRecords.Records.size()
     << " bb_records=" << Archive.AddRecords.BBRecords.size()
     << " phi_records=" << Archive.AddRecords.PhiRecords.size()
     << " delete_records=" << Archive.DeleteRecords.size()
     << " replace_records=" << Archive.ReplaceRecords.size() << "\n";

  OS << "[ReSanArchive] AddRecords\n";
  if (Archive.AddRecords.Records.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < Archive.AddRecords.Records.size(); ++I) {
      const AddRecord &Add = Archive.AddRecords.Records[I];
      OS << "  [add#" << I << "] sanitizer=" << San.name()
         << " kind=" << getAddRecordKindName(Add.Kind) << " anchor=";
      dumpValueRef(Add.Anchor, OS);
      OS << "\n";

      if (std::holds_alternative<AddRecord::PureExtra>(Add.ExtraRecord)) {
        OS << "    san_insts=" << Add.SanInsts->size() << "\n";
        dumpInstList(*Add.SanInsts, OS, "      ");
      } else if (const auto *Split =
                     std::get_if<AddRecord::SplitExtra>(&Add.ExtraRecord)) {
        OS << "    san_append_insts=" << Add.SanInsts->size() << " last_bb=";
        dumpBasicBlockRef(Split->SinkBB, OS);
        OS << "\n";
        dumpInstList(*Add.SanInsts, OS, "      ");
        dumpBasicBlockBody(Split->SinkBB, OS, "      ");
      } else {
        OS << "    <invalid add record variant>\n";
      }
    }
  }

  OS << "[ReSanArchive] PhiRecords\n";
  if (Archive.AddRecords.PhiRecords.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < Archive.AddRecords.PhiRecords.size(); ++I) {
      const PHIAddRecord &Record = Archive.AddRecords.PhiRecords[I];
      OS << "  [phi_record#" << I << "] target=";
      dumpBasicBlockRef(Record.TargetUsrBB, OS);
      OS << " phis=" << Record.SanPhis.size() << "\n";

      for (size_t J = 0; J < Record.SanPhis.size(); ++J) {
        OS << "    [phi#" << J << "] ";
        dumpValueRef(Record.SanPhis[J], OS);
        OS << "\n";
      }
    }
  }

  OS << "[ReSanArchive] BBRecords\n";
  if (Archive.AddRecords.BBRecords.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < Archive.AddRecords.BBRecords.size(); ++I) {
      const BlockAddRecord &Record = Archive.AddRecords.BBRecords[I];
      OS << "  [bb_record#" << I << "] parent=";
      dumpFunctionRef(Record.Parent, OS);
      OS << " insert_pt=";
      dumpBasicBlockRef(Record.InsertPt, OS);
      OS << " san_bbs=" << Record.SanBBs.size() << "\n";

      for (size_t J = 0; J < Record.SanBBs.size(); ++J) {
        OS << "    [bb#" << J << "] ";
        dumpBasicBlockRef(Record.SanBBs[J].SanBB, OS);
        OS << "\n";
        dumpBasicBlockBody(Record.SanBBs[J].SanBB, OS, "      ");
      }
    }
  }

  using ReplaceEntry =
      std::pair<Value *, const llvm::SmallVector<ReplaceRecord, 2> *>;
  std::vector<ReplaceEntry> ReplaceEntries;
  ReplaceEntries.reserve(Archive.ReplaceRecords.size());
  for (const auto &Entry : Archive.ReplaceRecords)
    ReplaceEntries.push_back({Entry.first, &Entry.second});

  std::sort(ReplaceEntries.begin(), ReplaceEntries.end(),
            [](const ReplaceEntry &L, const ReplaceEntry &R) {
              return pointerOrderKey(L.first) < pointerOrderKey(R.first);
            });

  OS << "[ReSanArchive] ReplaceRecords\n";
  if (ReplaceEntries.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < ReplaceEntries.size(); ++I) {
      const auto &[UserValue, RecordsPtr] = ReplaceEntries[I];
      const auto &Records = *RecordsPtr;
      OS << "  [replace#" << I << "] user=";
      dumpValueRef(UserValue, OS);
      OS << " records=" << Records.size() << "\n";

      for (size_t J = 0; J < Records.size(); ++J) {
        const ReplaceRecord &Record = Records[J];
        OS << "    [record#" << J << "] sanitizer=" << San.name() << " value=";
        dumpValueRef(Record.Replacer, OS);
        OS << "\n";
      }
    }
  }

  std::vector<Instruction *> DeleteEntries;
  DeleteEntries.reserve(Archive.DeleteRecords.size());
  for (Instruction *DeletedInst : Archive.DeleteRecords)
    DeleteEntries.push_back(DeletedInst);

  std::sort(DeleteEntries.begin(), DeleteEntries.end(),
            [](const Instruction *L, const Instruction *R) {
              return pointerOrderKey(L) < pointerOrderKey(R);
            });

  OS << "[ReSanArchive] DeleteRecords\n";
  if (DeleteEntries.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < DeleteEntries.size(); ++I) {
      Instruction *UserInst = DeleteEntries[I];
      OS << "  [delete#" << I << "] user_inst=";
      dumpValueRef(UserInst, OS);
      OS << "\n";
    }
  }
}

void validateArchive(const Sanitizer &San, const ReSanArchive &Archive,
                     const UserBBsMap &UserBBsMap) {
  std::string S;
  llvm::raw_string_ostream OS(S);

  static ::__xsan::desan::UserBBsSet CacheUserBBs;
  static const ::__xsan::desan::UserBBsMap *CacheUserBBsMap = nullptr;

  if (CacheUserBBsMap != &UserBBsMap) {
    CacheUserBBs.clear();
    for (const auto &[Func, UserBBs] : UserBBsMap)
      CacheUserBBs.insert(UserBBs.begin(), UserBBs.end());
    CacheUserBBsMap = &UserBBsMap;
  }

  for (const AddRecord &Add : Archive.AddRecords.Records) {
    const Instruction *Anchor = Add.Anchor;
    // Check anchor
    if (!Anchor) {
      OS << "[ReSan AddRecord] Sanitizer " << San.name()
         << ". Anchor == nullptr\n";
    } else if (!Anchor->getParent()) {
      OS << "[ReSan AddRecord] Sanitizer " << San.name() << ". Anchor:\n";
      dumpValueRef(Anchor, OS);
      OS << "\n has no parent block\n";
    } else if (!CacheUserBBs.contains(Anchor->getParent())) {
      OS << "[ReSan AddRecord] Sanitizer " << San.name() << ". Anchor:\n";
      dumpValueRef(Anchor, OS);
      OS << "\n is not in user blocks set\n";
    }
    if (Anchor && !__xsan::UserIR::is(*Anchor)) {
      OS << "[ReSan AddRecord] Sanitizer " << San.name() << ". Anchor:\n";
      dumpValueRef(Anchor, OS);
      OS << "\n is not a user instruction\n";
    }
    // Check record
    if (std::holds_alternative<AddRecord::PureExtra>(Add.ExtraRecord)) {
      if (Add.SanInsts->empty()) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". PureRecord has no instructions\n";
      }
      if (llvm::any_of(*Add.SanInsts, [](const Instruction &I) {
            return __xsan::UserIR::is(I);
          })) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". PureRecord has user instructions:\n";
        for (const Instruction &I : *Add.SanInsts) {
          dumpValueRef(&I, OS);
          OS << "\n";
        }
      }
    } else if (auto *Split =
                   std::get_if<AddRecord::SplitExtra>(&Add.ExtraRecord)) {
      if (Add.SanInsts->empty()) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord has no sanitizer instructions to append\n";
        continue;
      }
      if (llvm::any_of(*Add.SanInsts, [](const Instruction &I) {
            return __xsan::UserIR::is(I);
          })) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord has user instructions in SanInstsToAppend:\n";
        for (const Instruction &I : *Add.SanInsts) {
          dumpValueRef(&I, OS);
          OS << "\n";
        }
      }
      if (!Split->SinkBB) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord has null LastBB\n";
      } else if (CacheUserBBs.contains(Split->SinkBB)) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord LastBB:\n";
        dumpBasicBlockRef(Split->SinkBB, OS);
        OS << "\n is in user blocks set\n";
      } else {
        for (const Instruction &I : *Split->SinkBB) {
          if (__xsan::UserIR::is(I)) {
            OS << "[ReSan AddRecord] Sanitizer " << San.name()
               << ". SplitRecord sink block contains user instruction:\n";
            dumpValueRef(&I, OS);
            OS << "\n";
          }
        }
      }
    } else if (auto *Redirect =
                   std::get_if<AddRecord::RedirectExtra>(&Add.ExtraRecord)) {
      if (Add.SanInsts->empty()) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". RedirectRecord has no sanitizer instructions to append\n";
        continue;
      }
      if (llvm::any_of(*Add.SanInsts, [](const Instruction &I) {
            return __xsan::UserIR::is(I);
          })) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". RedirectRecord has user instructions in SanInsts:\n";
        for (const Instruction &I : *Add.SanInsts) {
          dumpValueRef(&I, OS);
          OS << "\n";
        }
      }
      if (!Redirect->SinkBB) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". RedirectRecord has null SinkBB\n";
      } else if (!CacheUserBBs.contains(Redirect->SinkBB)) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". RedirectRecord SinkBB:\n";
        dumpBasicBlockRef(Redirect->SinkBB, OS);
        OS << "\n is not in user blocks set\n";
      }
    } else {
      OS << "[ReSan AddRecord] Sanitizer " << San.name()
         << ". Record has an invalid variant type\n";
    }
  }

  for (const PHIAddRecord &Record : Archive.AddRecords.PhiRecords) {
    if (!Record.TargetUsrBB) {
      OS << "[ReSan PhiAddRecord] Sanitizer " << San.name()
         << ". TargetUsrBB == nullptr\n";
    } else if (!CacheUserBBs.contains(Record.TargetUsrBB)) {
      OS << "[ReSan PhiAddRecord] Sanitizer " << San.name()
         << ". TargetUsrBB is not in user blocks set:\n";
      dumpBasicBlockRef(Record.TargetUsrBB, OS);
      OS << "\n";
    }

    if (Record.SanPhis.empty()) {
      OS << "[ReSan PhiAddRecord] Sanitizer " << San.name()
         << ". has no PHIs\n";
    }
    for (const PHINode *Phi : Record.SanPhis) {
      if (!Phi) {
        OS << "[ReSan PhiAddRecord] Sanitizer " << San.name()
           << ". has null PHI\n";
      } else if (Phi->getParent()) {
        OS << "[ReSan PhiAddRecord] Sanitizer " << San.name()
           << ". PHI is still in a block:\n";
        dumpValueRef(Phi, OS);
        OS << "\n";
      } else if (__xsan::UserIR::is(*Phi)) {
        OS << "[ReSan PhiAddRecord] Sanitizer " << San.name()
           << ". PHI is an unexpected user instruction:\n";
        dumpValueRef(Phi, OS);
        OS << "\n";
      }
    }
  }

  for (const BlockAddRecord &Record : Archive.AddRecords.BBRecords) {
    if (!Record.Parent) {
      OS << "[ReSan AddArchive] BBRecord has null parent function\n";
    } else if (Record.Parent->isDeclaration()) {
      OS << "[ReSan AddArchive] BBRecord parent is declaration:\n";
      dumpFunctionRef(Record.Parent, OS);
      OS << "\n";
    } else if (!Record.Parent->getParent()) {
      OS << "[ReSan AddArchive] BBRecord parent has no parent module:\n";
      dumpFunctionRef(Record.Parent, OS);
      OS << "\n";
    }

    if (Record.InsertPt && !CacheUserBBs.contains(Record.InsertPt)) {
      OS << "[ReSan AddArchive] BBRecord insert point is not a user block:\n";
      dumpBasicBlockRef(Record.InsertPt, OS);
      OS << "\n";
    }

    for (const auto [BB, _] : Record.SanBBs) {
      if (!BB) {
        OS << "[ReSan AddArchive] has null BB in BBRecords\n";
      } else if (BB->getParent()) {
        OS << "[ReSan AddArchive] BB:\n";
        dumpBasicBlockRef(BB, OS);
        OS << "\n is still in a function\n";
      }
    }
  }

  for (const auto &[UserValue, Records] : Archive.ReplaceRecords) {
    if (!UserValue) {
      OS << "[ReSan ReplaceRecord] has null UserValue\n";
    } else if (!(isa<Instruction>(UserValue) &&
                 __xsan::UserIR::is(*cast<Instruction>(UserValue))) &&
               !isa<Argument>(UserValue)) {
      OS << "[ReSan ReplaceRecord] UserValue is not an user instruction or "
            "argument:\n";
      dumpValueRef(UserValue, OS);
      OS << "\n";
    }
    for (const ReplaceRecord &Record : Records) {
      if (!Record.Replacer) {
        OS << "[ReSan ReplaceRecord] Sanitizer " << San.name()
           << ". has null SanitizerValue\n";
      } else if (isa<Instruction>(Record.Replacer) &&
                 __xsan::UserIR::is(*cast<Instruction>(Record.Replacer)) &&
                 !(San.isUBSan() && __xsan::UBSanInst::is(
                                        *cast<Instruction>(Record.Replacer)))) {
        OS << "[ReSan ReplaceRecord] Sanitizer " << San.name()
           << ". SanitizerValue is an unexpected user instruction:\n";
        dumpValueRef(Record.Replacer, OS);
        OS << "\n";
      } else if (isa<Argument>(Record.Replacer)) {
        OS << "[ReSan ReplaceRecord] Sanitizer " << San.name()
           << ". SanitizerValue is an user instruction or argument:\n";
        dumpValueRef(Record.Replacer, OS);
        OS << "\n";
      }
    }
  }

  for (Instruction *DeletedInst : Archive.DeleteRecords) {
    if (!DeletedInst) {
      OS << "[ReSan DeleteRecord] has null DeletedInst\n";
    } else if (!(isa<Instruction>(DeletedInst) &&
                 __xsan::UserIR::is(*cast<Instruction>(DeletedInst)))) {
      OS << "[ReSan DeleteRecord] DeletedInst is not an user instruction:\n";
      dumpValueRef(DeletedInst, OS);
      OS << "\n";
    }
  }

  OS.flush();
  if (!S.empty())
    errs() << "ReSanArchive validation failed with the following issues:\n"
           << S;
}

} // namespace __xsan::desan
