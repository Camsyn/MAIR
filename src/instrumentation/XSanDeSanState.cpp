#include "XSanDeSanState.h"
#include "Utils/MetaDataUtils.h"
#include "Utils/Options.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>

using namespace llvm;

namespace __xsan::desan {
void requireOrDie(bool Condition, const Twine &Message) {
  if (!Condition)
    report_fatal_error(Message, false);
}

void markUserIR(Module &M) {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
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

bool isStageIR(StageKind Stage, const llvm::Instruction &I) {
  switch (Stage) {
  case StageKind::UBSan:
    return __xsan::UBSanInst::is(I);
  case StageKind::ASan:
    return __xsan::ASanInst::is(I);
  case StageKind::TSan:
    return __xsan::TSanInst::is(I);
  case StageKind::MSan:
    return __xsan::MSanInst::is(I);
  default:
    return false;
  }
  llvm_unreachable("invalid stage");
}

void setStageIR(StageKind Stage, llvm::Instruction &I) {
  switch (Stage) {
  case StageKind::UBSan:
    __xsan::UBSanInst::set(I);
    return;
  case StageKind::ASan:
    __xsan::ASanInst::set(I);
    return;
  case StageKind::TSan:
    __xsan::TSanInst::set(I);
    return;
  case StageKind::MSan:
    __xsan::MSanInst::set(I);
    return;
  default:
    llvm_unreachable("invalid stage");
  }
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

bool replaceNonStageIRUses(StageKind Stage, Value &From, Value &To) {
  bool Changed = false;
  From.replaceUsesWithIf(&To, [&](Use &U) -> bool {
    auto *UserI = dyn_cast<Instruction>(U.getUser());
    const bool IsStageUse = UserI && isStageIR(Stage, *UserI);
    Changed |= !IsStageUse;
    return !IsStageUse;
  });
  return Changed;
}

} // namespace __xsan::desan
