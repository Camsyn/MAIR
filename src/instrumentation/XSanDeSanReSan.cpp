#include "Utils/Options.h"
#include "XSanDeSanState.h"

using namespace llvm;

namespace __xsan::desan {

// We need ReplaceArchive to redirect instruction anchor, and DeleteArchive to
// add the sentry anchor into it.
static void replayAdd(Module &M, AddArchive &Archive,
                      DeleteArchive &DeleteArchive,
                      const ReplaceArchive &ReplaceArchive) {
}

static void replayReplace(Module &M, ReplaceArchive &Archive) {
}

static void replayDelete(Module &M, DeleteArchive &Archive) {
}

static void replayFunction(Module &M, FunctionArchive &Archive) {
}

// Both replayAdd and replayReplace use the ReSanArchive.
// So we do the validation before both of them.
static void validateReplaceArchive(const ReplaceArchive &Archive) {
}

void runReSan(Module &M, ReSanArchive &Archive) {
  validateReplaceArchive(Archive.ReplaceRecords);

  replayAdd(M, Archive.AddRecords, Archive.DeleteRecords,
            Archive.ReplaceRecords);
  replayReplace(M, Archive.ReplaceRecords);
  replayDelete(M, Archive.DeleteRecords);

  replayFunction(M, Archive.FunctionRecords);
}

} // namespace __xsan::desan
