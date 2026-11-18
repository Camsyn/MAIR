#include "MAIR/MAIRStatLog.h"

#include "MAIR/MAIR.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace mair {
namespace {

cl::opt<bool> ClMAIRStats("mair-stats",
                          cl::desc("Print a readable MAIR pass summary"));

const char *const FieldNames[] = {"in",           "preheader",
                                  "exit",         "range",
                                  "periodic",     "merged",
                                  "intra",        "inter",
                                  "eliminated",   "hoisted",
                                  "sunk",         "exit-trip",
                                  "exit-counter", "wrap-index",
                                  "rej-barrier",  "rej-nonaffine",
                                  "rej-cond",     "rej-multiexit",
                                  "rej-nocount",  "rej-wrap",
                                  "rej-other",    "lowered",
                                  "native",       "multi",
                                  "vector",       "symbolic",
                                  "shared",       "fallback",
                                  "scev",         "lvi"};
constexpr unsigned FieldCount = std::size(FieldNames);
static_assert(static_cast<unsigned>(MAIRStatField::Lvi) + 1 == FieldCount,
              "every MAIR stat field needs a name");

struct Visit {
  std::string Pass;
  std::array<uint64_t, FieldCount> Counts{};
};

std::string Pipeline;
// The visits of each function, in first-visit order.
std::vector<std::vector<Visit>> Functions;

// A replacement need not be a Function and must not inherit its visits.
struct FunctionIndexConfig : ValueMapConfig<const Value *> {
  enum { FollowRAUW = false };
};
ValueMap<const Value *, unsigned, FunctionIndexConfig> FunctionIndex(0);

struct ActiveVisit {
  unsigned Function = 0;
  unsigned Visit = 0;
};

std::optional<ActiveVisit> Current;

unsigned findVisit(std::vector<Visit> &Visits, StringRef Pass) {
  for (unsigned Index = 0; Index < Visits.size(); ++Index)
    if (Visits[Index].Pass == Pass)
      return Index;
  Visits.push_back(Visit{Pass.str(), {}});
  return Visits.size() - 1;
}

void printVisit(raw_ostream &OS, StringRef Pass,
                const std::array<uint64_t, FieldCount> &Counts) {
  if (all_of(Counts, [](uint64_t Count) { return Count == 0; }))
    return;
  OS << format("    %-16s", Pass.str().c_str());
  for (unsigned Index = 0; Index < FieldCount; ++Index)
    if (Counts[Index] != 0)
      OS << "  " << FieldNames[Index] << " " << Counts[Index];
  OS << "\n";
}

void printLog() {
  if (!ClMAIRStats)
    return;

  errs() << "MAIR stats\n";
  if (Functions.empty()) {
    errs() << "  none\n";
    return;
  }
  if (!Pipeline.empty())
    errs() << "  pipeline " << Pipeline << "\n";
  errs() << "  functions " << Functions.size() << "\n";
  errs() << "  total\n";

  MapVector<std::string, std::array<uint64_t, FieldCount>, StringMap<unsigned>>
      Totals;
  for (const std::vector<Visit> &Visits : Functions) {
    for (const Visit &Item : Visits) {
      auto &Sum = Totals[Item.Pass];
      for (unsigned Index = 0; Index < FieldCount; ++Index)
        Sum[Index] += Item.Counts[Index];
    }
  }
  for (const auto &[Pass, Counts] : Totals)
    printVisit(errs(), Pass, Counts);
}

struct LogPrinter {
  ~LogPrinter() { printLog(); }
};

LogPrinter Printer;

} // namespace

void noteMAIRPipeline(StringRef Name) {
  if (!ClMAIRStats || !Pipeline.empty())
    return;
  Pipeline = Name.str();
}

bool mairStatsEnabled() { return ClMAIRStats; }

void MAIRStatScope::begin(StringRef Pass, unsigned Ordinal, const MAIRGraph &G,
                          uint64_t NumInputs) {
  assert(ClMAIRStats && "a MAIR stat scope begins only under -mair-stats");
  assert(!Current && "MAIR stat scope is not re-entrant");
  const Function *Func = G.getRoot().getFunction();
  std::string Label =
      Ordinal == 0 ? Pass.str() : (Pass + "#" + Twine(Ordinal)).str();
  auto [It, Inserted] = FunctionIndex.insert({Func, Functions.size()});
  if (Inserted)
    Functions.emplace_back();
  unsigned FnIndex = It->second;
  Current = ActiveVisit{FnIndex, findVisit(Functions[FnIndex], Label)};
  add(MAIRStatField::In, NumInputs);
}

MAIRStatScope::~MAIRStatScope() { Current.reset(); }

void MAIRStatScope::add(MAIRStatField Field, uint64_t N) {
  if (!ClMAIRStats || N == 0)
    return;
  assert(Current && "MAIR stat update outside a pass");
  Functions[Current->Function][Current->Visit]
      .Counts[static_cast<unsigned>(Field)] += N;
}

} // namespace mair
