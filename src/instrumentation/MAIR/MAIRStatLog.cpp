#include "MAIR/MAIRStatLog.h"

#include "MAIR/MAIR.h"

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

enum Field {
  In = 0,
  Preheader,
  Exit,
  Range,
  Periodic,
  Merged,
  Intra,
  Inter,
  Eliminated,
  Hoisted,
  Sunk,
  FieldCount
};

struct Visit {
  std::string Pass;
  std::array<uint64_t, FieldCount> Counts{};
};

struct FunctionStats {
  std::string Name;
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

int fieldIndex(StringRef Name) {
  if (Name == "in")
    return In;
  if (Name == "preheader")
    return Preheader;
  if (Name == "exit")
    return Exit;
  if (Name == "range")
    return Range;
  if (Name == "periodic")
    return Periodic;
  if (Name == "merged")
    return Merged;
  if (Name == "intra")
    return Intra;
  if (Name == "inter")
    return Inter;
  if (Name == "eliminated")
    return Eliminated;
  if (Name == "hoisted")
    return Hoisted;
  if (Name == "sunk")
    return Sunk;
  return -1;
}

const char *fieldName(int Index) {
  static const char *Names[] = {"in",         "preheader", "exit",  "range",
                                "periodic",   "merged",    "intra", "inter",
                                "eliminated", "hoisted",   "sunk"};
  return Names[Index];
}

bool hasAction(const std::array<uint64_t, FieldCount> &Counts) {
  for (int Index = Preheader; Index < FieldCount; ++Index)
    if (Counts[Index] != 0)
      return true;
  return false;
}

Visit &findVisit(FunctionStats &Fn, StringRef Pass) {
  for (Visit &Item : Fn.Visits)
    if (Item.Pass == Pass)
      return Item;
  Fn.Visits.push_back(Visit{Pass.str(), {}});
  return Fn.Visits.back();
}

void printCounts(raw_ostream &OS,
                 const std::array<uint64_t, FieldCount> &Counts,
                 bool ActionsOnly) {
  for (int Index = 0; Index < FieldCount; ++Index) {
    if (Counts[Index] == 0)
      continue;
    if (ActionsOnly && Index == In)
      continue;
    OS << "  " << fieldName(Index) << " " << Counts[Index];
  }
}

void printVisit(raw_ostream &OS, StringRef Pass,
                const std::array<uint64_t, FieldCount> &Counts,
                bool ActionsOnly) {
  if (ActionsOnly && !hasAction(Counts))
    return;
  bool Any = false;
  for (uint64_t Count : Counts)
    if (Count != 0)
      Any = true;
  if (!Any)
    return;
  OS << format("    %-16s", Pass.str().c_str());
  printCounts(OS, Counts, ActionsOnly);
  OS << "\n";
}

std::array<uint64_t, FieldCount> totalCounts(StringRef Pass) {
  std::array<uint64_t, FieldCount> Sum{};
  for (const FunctionStats &Fn : Functions)
    for (const Visit &Item : Fn.Visits)
      if (Item.Pass == Pass)
        for (int Index = 0; Index < FieldCount; ++Index)
          Sum[Index] += Item.Counts[Index];
  return Sum;
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

  StringMap<unsigned> SeenPasses;
  std::vector<std::string> PassOrder;
  for (const FunctionStats &Fn : Functions) {
    for (const Visit &Item : Fn.Visits) {
      if (SeenPasses.contains(Item.Pass))
        continue;
      SeenPasses[Item.Pass] = 1;
      PassOrder.push_back(Item.Pass);
    }
  }
  for (const std::string &Pass : PassOrder)
    printVisit(errs(), Pass, totalCounts(Pass), /*ActionsOnly=*/false);
}

struct LogPrinter {
  ~LogPrinter() { printLog(); }
};

LogPrinter Printer;

} // namespace

bool mairStatsEnabled() { return ClMAIRStats; }

void noteMAIRPipeline(StringRef Name) {
  if (!ClMAIRStats || !Pipeline.empty())
    return;
  Pipeline = Name.str();
}

StringRef mairStatFunctionName(const MAIRGraph &G) {
  const MAIRGraph *Cur = &G;
  while (const MAIRGraph *Parent = Cur->getParent())
    Cur = Parent;
  if (const Function *F = Cur->getFunction()) {
    if (!F->getName().empty())
      return F->getName();
    return "<unnamed>";
  }
  return "<unknown>";
}

MAIRStatScope::MAIRStatScope(StringRef Pass, unsigned Ordinal, StringRef Func) {
  if (!ClMAIRStats)
    return;
  assert(!Current && "MAIR stat scope is not re-entrant");
  std::string Label =
      Ordinal == 0 ? Pass.str() : (Pass + "#" + Twine(Ordinal)).str();
  auto It = FunctionIndex.find(Func);
  if (It == FunctionIndex.end()) {
    FunctionIndex[Func] = Functions.size();
    Functions.push_back(FunctionStats{Func.str(), {}});
    It = FunctionIndex.find(Func);
  }
  FunctionStats &Fn = Functions[It->second];
  findVisit(Fn, Label);
  unsigned VisitIndex = 0;
  for (const Visit &Item : Fn.Visits) {
    if (Item.Pass == Label)
      break;
    ++VisitIndex;
  }
  static ActiveVisit Storage;
  Storage = ActiveVisit{It->second, VisitIndex};
  Current = &Storage;
}

MAIRStatScope::~MAIRStatScope() {
  if (!ClMAIRStats)
    return;
  Current = nullptr;
}

void MAIRStatScope::add(StringRef Field, uint64_t N) {
  if (!ClMAIRStats || N == 0)
    return;
  assert(Current && "MAIR stat update outside a pass");
  int Index = fieldIndex(Field);
  assert(Index >= 0 && "unknown MAIR stat field");
  Functions[Current->Function].Visits[Current->Visit].Counts[Index] += N;
}

} // namespace mair
