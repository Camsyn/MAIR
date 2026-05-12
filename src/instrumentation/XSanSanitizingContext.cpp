#include "XSanSanitizingContext.h"
#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
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

static void dumpBBAnchor(AddArchive::BBAnchor Anchor,
                         AddArchive::BBAnchorType AnchorType, raw_ostream &OS) {
  switch (AnchorType) {
  case AddArchive::FunctionAnchor:
    OS << "function ";
    dumpFunctionRef(static_cast<const Function *>(Anchor), OS);
    return;
  case AddArchive::BasicBlockAnchor:
    OS << "basic_block ";
    dumpBasicBlockRef(static_cast<const BasicBlock *>(Anchor), OS);
    return;
  }

  OS << "invalid ptr=" << Anchor
     << " type=" << static_cast<unsigned>(AnchorType);
}

} // namespace

void requireOrDie(bool Condition, const Twine &Message) {
  if (!Condition)
    report_fatal_error(Message, false);
}

void SanitizingContext::clearArchives() { SanitizerArchives.clear(); }

SanitizerArchive &SanitizingContext::addArchive(const Sanitizer &San) {
  for (const SanitizerArchive &Entry : SanitizerArchives)
    if (Entry.San.sameAs(San))
      report_fatal_error(Twine("duplicate ReSanArchive for sanitizer ") +
                         San.name());

  return SanitizerArchives.emplace_back(San);
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
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (!__xsan::UBSanInst::is(I))
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
     << " san_bb_anchors=" << Archive.AddRecords.SanBBMap.size()
     << " delete_records=" << Archive.DeleteRecords.size()
     << " replace_records=" << Archive.ReplaceRecords.size() << "\n";

  OS << "[ReSanArchive] AddRecords\n";
  if (Archive.AddRecords.Records.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < Archive.AddRecords.Records.size(); ++I) {
      const auto &[Kind, Anchor, Record] = Archive.AddRecords.Records[I];
      OS << "  [add#" << I << "] sanitizer=" << San.name()
         << " kind=" << getAddRecordKindName(Kind) << " anchor=";
      dumpValueRef(Anchor, OS);
      OS << "\n";

      if (const auto *Pure = std::get_if<AddRecord::PureRecord>(&Record)) {
        OS << "    san_insts=" << Pure->SanInsts->size() << "\n";
        dumpInstList(*Pure->SanInsts, OS, "      ");
      } else if (const auto *Split =
                     std::get_if<AddRecord::SplitRecord>(&Record)) {
        OS << "    san_append_insts=" << Split->SanInstsToAppend->size()
           << " last_bb=";
        dumpBasicBlockRef(Split->LastBB, OS);
        OS << "\n";
        dumpInstList(*Split->SanInstsToAppend, OS, "      ");
      } else {
        OS << "    <invalid add record variant>\n";
      }
    }
  }

  using SanBBEntry =
      std::pair<AddArchive::BBAnchor,
                std::pair<AddArchive::BBAnchorType, std::vector<BasicBlock *>>>;
  std::vector<SanBBEntry> SanBBEntries;
  SanBBEntries.reserve(Archive.AddRecords.SanBBMap.size());
  for (const auto &Entry : Archive.AddRecords.SanBBMap)
    SanBBEntries.push_back({Entry.first, Entry.second});

  std::sort(SanBBEntries.begin(), SanBBEntries.end(),
            [](const SanBBEntry &L, const SanBBEntry &R) {
              if (L.second.first != R.second.first)
                return L.second.first < R.second.first;
              return pointerOrderKey(L.first) < pointerOrderKey(R.first);
            });

  OS << "[ReSanArchive] SanBBMap\n";
  if (SanBBEntries.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < SanBBEntries.size(); ++I) {
      const auto &[Anchor, Pair] = SanBBEntries[I];
      const auto &[AnchorType, BBs] = Pair;
      OS << "  [bb_anchor#" << I << "] anchor=";
      dumpBBAnchor(Anchor, AnchorType, OS);
      OS << " san_bbs=" << BBs.size() << "\n";

      for (size_t J = 0; J < BBs.size(); ++J) {
        OS << "    [bb#" << J << "] ";
        dumpBasicBlockRef(BBs[J], OS);
        OS << "\n";
        dumpBasicBlockBody(BBs[J], OS, "      ");
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
        dumpValueRef(Record.SanitizerValue, OS);
        OS << "\n";
      }
    }
  }

  using DeleteEntry =
      std::pair<Instruction *, const llvm::SmallVector<DeleteRecord, 2> *>;
  std::vector<DeleteEntry> DeleteEntries;
  DeleteEntries.reserve(Archive.DeleteRecords.size());
  for (const auto &Entry : Archive.DeleteRecords)
    DeleteEntries.push_back({Entry.first, &Entry.second});

  std::sort(DeleteEntries.begin(), DeleteEntries.end(),
            [](const DeleteEntry &L, const DeleteEntry &R) {
              return pointerOrderKey(L.first) < pointerOrderKey(R.first);
            });

  OS << "[ReSanArchive] DeleteRecords\n";
  if (DeleteEntries.empty()) {
    OS << "  <empty>\n";
  } else {
    for (size_t I = 0; I < DeleteEntries.size(); ++I) {
      const auto &[UserInst, RecordsPtr] = DeleteEntries[I];
      const auto &Records = *RecordsPtr;
      OS << "  [delete#" << I << "] user_inst=";
      dumpValueRef(UserInst, OS);
      OS << " records=" << Records.size() << "\n";

      for (size_t J = 0; J < Records.size(); ++J) {
        OS << "    [record#" << J << "] sanitizer=" << San.name() << "\n";
      }
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

  for (const auto &[Kind, Anchor, Record] : Archive.AddRecords.Records) {
    // Check anchor
    if (!Anchor) {
      OS << "[ReSan AddRecord] Sanitizer " << San.name()
         << ". Anchor == nullptr\n";
    } else {
      if (!Anchor->getParent()) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name() << ". Anchor:\n";
        dumpValueRef(Anchor, OS);
        OS << "\n has no parent block\n";
      } else if (!CacheUserBBs.contains(Anchor->getParent())) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name() << ". Anchor:\n";
        dumpValueRef(Anchor, OS);
        OS << "\n is not in user blocks set\n";
      }
      if (!__xsan::UserIR::is(*Anchor)) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name() << ". Anchor:\n";
        dumpValueRef(Anchor, OS);
        OS << "\n is not a user instruction\n";
      }
    }
    // Check record
    if (auto *Pure = std::get_if<AddRecord::PureRecord>(&Record)) {
      if (Pure->SanInsts->empty()) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". PureRecord has no instructions\n";
      }
      if (llvm::any_of(*Pure->SanInsts, [](const Instruction &I) {
            return __xsan::UserIR::is(I);
          })) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". PureRecord has user instructions:\n";
        for (const Instruction &I : *Pure->SanInsts) {
          dumpValueRef(&I, OS);
          OS << "\n";
        }
      }
    } else if (auto *Split = std::get_if<AddRecord::SplitRecord>(&Record)) {
      if (Split->SanInstsToAppend->empty()) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord has no sanitizer instructions to append\n";
        continue;
      }
      if (llvm::any_of(*Split->SanInstsToAppend, [](const Instruction &I) {
            return __xsan::UserIR::is(I);
          })) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord has user instructions in SanInstsToAppend:\n";
        for (const Instruction &I : *Split->SanInstsToAppend) {
          dumpValueRef(&I, OS);
          OS << "\n";
        }
      }
      if (!Split->LastBB) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord has null LastBB\n";
      } else if (CacheUserBBs.contains(Split->LastBB)) {
        OS << "[ReSan AddRecord] Sanitizer " << San.name()
           << ". SplitRecord LastBB:\n";
        dumpBasicBlockRef(Split->LastBB, OS);
        OS << "\n is in user blocks set\n";
      }
    } else {
      OS << "[ReSan AddRecord] Sanitizer " << San.name()
         << ". Record has an invalid variant type\n";
    }
  }

  for (const auto &[Anchor, Pair] : Archive.AddRecords.SanBBMap) {
    // Check anchor
    auto &[AnchorType, BBs] = Pair;
    if (AnchorType == AddArchive::FunctionAnchor) {
      const auto *F = static_cast<Function *>(Anchor);
      if (!F) {
        OS << "[ReSan AddArchive] BB Anchor == nullptr\n";
      } else if (F->isDeclaration()) {
        OS << "[ReSan AddArchive] BB Anchor in a declaration function:\n";
        dumpFunctionRef(F, OS);
        OS << "\n";
      } else if (!F->getParent()) {
        OS << "[ReSan AddArchive] BB Anchor function:\n";
        dumpFunctionRef(F, OS);
        OS << "\n has no parent module\n";
      }
    } else if (AnchorType == AddArchive::BasicBlockAnchor) {
      const auto *BB = static_cast<BasicBlock *>(Anchor);
      if (!BB) {
        OS << "[ReSan AddArchive] BB Anchor == nullptr\n";
      } else {
        if (!CacheUserBBs.contains(BB)) {
          OS << "[ReSan AddArchive] BB Anchor:\n";
          dumpBasicBlockRef(BB, OS);
          OS << "\n is not in user blocks set\n";
        }
      }
    } else {
      OS << "[ReSan AddArchive] BB Anchor has an invalid variant type\n";
    }

    // Check BBs
    for (const BasicBlock *BB : BBs) {
      if (!BB) {
        OS << "[ReSan AddArchive] has null BB in SanBBMap\n";
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
      if (!Record.SanitizerValue) {
        OS << "[ReSan ReplaceRecord] Sanitizer " << San.name()
           << ". has null SanitizerValue\n";
      } else if (isa<Instruction>(Record.SanitizerValue) &&
                 __xsan::UserIR::is(
                     *cast<Instruction>(Record.SanitizerValue)) &&
                 !(San.isUBSan() && __xsan::UBSanInst::is(*cast<Instruction>(
                                        Record.SanitizerValue)))) {
        OS << "[ReSan ReplaceRecord] Sanitizer " << San.name()
           << ". SanitizerValue is an unexpected user instruction:\n";
        dumpValueRef(Record.SanitizerValue, OS);
        OS << "\n";
      } else if (isa<Argument>(Record.SanitizerValue)) {
        OS << "[ReSan ReplaceRecord] Sanitizer " << San.name()
           << ". SanitizerValue is an user instruction or argument:\n";
        dumpValueRef(Record.SanitizerValue, OS);
        OS << "\n";
      }
    }
  }

  for (const auto &[DeletedInst, Records] : Archive.DeleteRecords) {
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
