#include "Utils/Options.h"
#include "XSanDeSanState.h"

using namespace llvm;

namespace __xsan::desan {

static void restoreFunction(StageKind Stage, FunctionArchive &Archive,
                            Module &M, const UserBBsMap &UserBBsMap) {
}

static void restoreDelete(StageKind Stage, DeleteArchive &Archive,
                          std::vector<DeleteEvent> &Events) {
}

static void restoreReplace(StageKind Stage, ReplaceArchive &Archive,
                           std::vector<ReplaceEvent> &Events) {
}

static void restoreAdd(StageKind Stage, AddArchive &Archive, Module &M,
                       const UserBBsMap &UserBBsMap) {
}

static void restoreUbsanArithmetic(ReSanArchive &Archive, Module &M) {
}

void runDeSan(Module &M, StageKind Stage, ReSanArchive &Archive,
              UserBBsMap &UserBBsMap) {
  if (Stage == StageKind::UBSan) {
    restoreUbsanArithmetic(Archive, M);
    return;
  }

  auto DeleteEvents = getDeleteEvents();
  auto ReplaceEvents = getReplaceEvents();
  auto DeleteBlocks = getDeleteBlocks();

  restoreFunction(Stage, Archive.FunctionRecords, M, UserBBsMap);
  restoreDelete(Stage, Archive.DeleteRecords, DeleteEvents);
  restoreReplace(Stage, Archive.ReplaceRecords, ReplaceEvents);
  restoreAdd(Stage, Archive.AddRecords, M, UserBBsMap);
}

} // namespace __xsan::desan
