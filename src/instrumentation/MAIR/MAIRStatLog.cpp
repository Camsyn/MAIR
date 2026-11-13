#include "MAIR/MAIRStatLog.h"

#include "MAIR/MAIR.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
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

const char *const FieldNames[] = {"in",         "preheader", "exit",  "range",
                                  "periodic",   "merged",    "intra", "inter",
                                  "eliminated", "hoisted",   "sunk"};
constexpr unsigned FieldCount = std::size(FieldNames);
static_assert(static_cast<unsigned>(MAIRStatField::Sunk) + 1 == FieldCount,
              "every MAIR stat field needs a name");

struct Visit {
  std::string Pass;
  std::array<uint64_t, FieldCount> Counts{};
};

std::string Pipeline;
// The visits of each function, in first-visit order.
std::vector<std::vector<Visit>> Functions;
StringMap<unsigned> FunctionIndex;

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

// Child loop graphs do not store the function; the root graph does.
static StringRef getFunctionName(const MAIRGraph &G) {
  const Function *F = G.getRoot().getFunction();
  if (!F)
    return "<unknown>";
  return F->getName().empty() ? "<unnamed>" : F->getName();
}

bool mairStatsEnabled() { return ClMAIRStats; }

void MAIRStatScope::begin(StringRef Pass, unsigned Ordinal, const MAIRGraph &G,
                          uint64_t NumInputs) {
  assert(ClMAIRStats && "a MAIR stat scope begins only under -mair-stats");
  assert(!Current && "MAIR stat scope is not re-entrant");
  StringRef Func = getFunctionName(G);
  std::string Label =
      Ordinal == 0 ? Pass.str() : (Pass + "#" + Twine(Ordinal)).str();
  auto [It, Inserted] = FunctionIndex.try_emplace(Func, Functions.size());
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
