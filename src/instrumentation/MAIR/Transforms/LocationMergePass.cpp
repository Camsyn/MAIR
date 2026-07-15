#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

using namespace llvm;

#define DEBUG_TYPE "mair-locationmerge"

STATISTIC(NumMergedAccesses, "Number of accesses merged by location merging");

namespace mair {

namespace {

using AccessGroup = SmallVector<MemoryAccess *, 8>;

bool hasConcreteRange(const MemoryAccess &Access) {
  const AbstractAccess *Location = Access.getContiguousRegion();
  return Location && Location->getOffset().isConstant() &&
         Location->getSize().isConstant();
}

void eraseAccess(Observation &Obs, MemoryAccess &Access) {
  auto It = llvm::find_if(Obs.accesses(), [&](MemoryAccess &Candidate) {
    return &Candidate == &Access;
  });
  assert(It != Obs.end() && "memory access does not belong to observation");
  Obs.erase(It);
}

bool mergeLocations(Observation &Obs) {
  DenseMap<MemoryObject *, AccessGroup> Groups;
  for (MemoryAccess &Access : Obs)
    if (Access.isContiguous())
      Groups[&Access.getMemoryObject()].push_back(&Access);

  bool Changed = false;
  for (auto &Entry : Groups) {
    AccessGroup &Accesses = Entry.second;
    llvm::stable_sort(
        Accesses, [](const MemoryAccess *LHS, const MemoryAccess *RHS) {
          bool LConcrete = hasConcreteRange(*LHS);
          bool RConcrete = hasConcreteRange(*RHS);
          if (LConcrete != RConcrete)
            return LConcrete;
          if (!LConcrete)
            return false;
          return LHS->getContiguousRegion()->getOffset().getConstant() <
                 RHS->getContiguousRegion()->getOffset().getConstant();
        });

    size_t ConcreteEnd = llvm::find_if(Accesses,
                                       [](const MemoryAccess *Access) {
                                         return !hasConcreteRange(*Access);
                                       }) -
                         Accesses.begin();
    MemoryAccess *Current = Accesses.front();
    for (size_t I = 1; I < ConcreteEnd;) {
      MemoryAccess *Next = Accesses[I];
      if (!Current->mergeWith(*Next)) {
        Current = Next;
        ++I;
        continue;
      }
      eraseAccess(Obs, *Next);
      Accesses.erase(Accesses.begin() + I);
      --ConcreteEnd;
      Changed = true;
      ++NumMergedAccesses;
    }

    for (size_t I = ConcreteEnd; I < Accesses.size(); ++I) {
      Current = Accesses[I];
      for (size_t J = I + 1; J < Accesses.size();) {
        MemoryAccess *Next = Accesses[J];
        if (*Current->getContiguousRegion() != *Next->getContiguousRegion()) {
          ++J;
          continue;
        }
        bool Merged = Current->mergeWith(*Next);
        if (!Merged) {
          ++J;
          continue;
        }
        eraseAccess(Obs, *Next);
        Accesses.erase(Accesses.begin() + J);
        Changed = true;
        ++NumMergedAccesses;
      }
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses LocationMergePass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
  (void)AM;
  bool Changed = false;
  for (Observation &Obs : G)
    Changed |= mergeLocations(Obs);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
