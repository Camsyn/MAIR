#include "MAIR/MAIRStatLog.h"

#include "MAIR/MAIR.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
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

struct FunctionStats {
  std::vector<Visit> Visits;
};

std::string Pipeline;
std::vector<FunctionStats> Functions;
StringMap<unsigned> FunctionIndex;
bool Printed = false;

struct ActiveVisit {
  unsigned Function = 0;
  unsigned Visit = 0;
};

ActiveVisit *Current = nullptr;

unsigned findVisit(FunctionStats &Fn, StringRef Pass) {
  for (unsigned Index = 0; Index < Fn.Visits.size(); ++Index)
    if (Fn.Visits[Index].Pass == Pass)
      return Index;
  Fn.Visits.push_back(Visit{Pass.str(), {}});
  return Fn.Visits.size() - 1;
}

void printCounts(raw_ostream &OS,
                 const std::array<uint64_t, FieldCount> &Counts) {
  for (unsigned Index = 0; Index < FieldCount; ++Index) {
    if (Counts[Index] == 0)
      continue;
    OS << "  " << FieldNames[Index] << " " << Counts[Index];
  }
}

void printVisit(raw_ostream &OS, StringRef Pass,
                const std::array<uint64_t, FieldCount> &Counts) {
  if (all_of(Counts, [](uint64_t Count) { return Count == 0; }))
    return;
  OS << format("    %-16s", Pass.str().c_str());
  printCounts(OS, Counts);
  OS << "\n";
}

void printLog() {
  if (Printed)
    return;
  Printed = true;
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
  for (const FunctionStats &Fn : Functions) {
    for (const Visit &Item : Fn.Visits) {
      auto &Sum = Totals[Item.Pass];
      for (unsigned Index = 0; Index < FieldCount; ++Index)
        Sum[Index] += Item.Counts[Index];
    }
  }
  for (const auto &Item : Totals)
    printVisit(errs(), Item.first, Item.second);
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

MAIRStatScope::MAIRStatScope(StringRef Pass, unsigned Ordinal,
                             const MAIRGraph &G, uint64_t NumInputs) {
  if (!ClMAIRStats)
    return;
  assert(!Current && "MAIR stat scope is not re-entrant");
  StringRef Func = getFunctionName(G);
  std::string Label =
      Ordinal == 0 ? Pass.str() : (Pass + "#" + Twine(Ordinal)).str();
  auto It = FunctionIndex.find(Func);
  if (It == FunctionIndex.end()) {
    FunctionIndex[Func] = Functions.size();
    Functions.push_back(FunctionStats{});
    It = FunctionIndex.find(Func);
  }
  FunctionStats &Fn = Functions[It->second];
  unsigned VisitIndex = findVisit(Fn, Label);
  static ActiveVisit Storage;
  Storage = ActiveVisit{It->second, VisitIndex};
  Current = &Storage;
  add(MAIRStatField::In, NumInputs);
}

MAIRStatScope::~MAIRStatScope() {
  if (!ClMAIRStats)
    return;
  Current = nullptr;
}

void MAIRStatScope::add(MAIRStatField Field, uint64_t N) {
  if (!ClMAIRStats || N == 0)
    return;
  assert(Current && "MAIR stat update outside a pass");
  Functions[Current->Function]
      .Visits[Current->Visit]
      .Counts[static_cast<unsigned>(Field)] += N;
}

} // namespace mair
