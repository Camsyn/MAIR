#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

using namespace llvm;

#define DEBUG_TYPE "mair-locationmerge"

STATISTIC(NumMergedAccesses, "Number of accesses merged by location merging");
STATISTIC(NumInputAccesses,
          "Number of memory accesses entering location merging");

namespace mair {

namespace {

using AccessGroup = SmallVector<MemoryAccess *, 8>;

bool hasConcreteRange(const MemoryAccess &Access) {
  const AbstractAccess *Location = Access.getContiguousRegion();
  return Location && Location->getOffset().isConstant() &&
         Location->getSize().isConstant();
}

bool mergeLocations(Observation &Obs) {
  // Concrete ranges are sorted so adjacent/overlapping regions merge in one
  // sweep. Symbolic and masked regions lack that ordering and use pairwise
  // comparison only after the concrete prefix has converged.
  DenseMap<MemoryObject *, AccessGroup> Groups;
  for (MemoryAccess &Access : Obs)
    if (!Access.isPeriodic())
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
      Obs.erase(*Next);
      Accesses.erase(Accesses.begin() + I);
      --ConcreteEnd;
      Changed = true;
      ++NumMergedAccesses;
    }

    for (size_t I = ConcreteEnd; I < Accesses.size(); ++I) {
      Current = Accesses[I];
      for (size_t J = I + 1; J < Accesses.size();) {
        MemoryAccess *Next = Accesses[J];
        bool Merged = Current->mergeWith(*Next);
        if (!Merged) {
          ++J;
          continue;
        }
        Obs.erase(*Next);
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
#if LLVM_ENABLE_STATS
  NumInputAccesses += MAIRStat::compute(G).getNumMemoryAccesses();
#else
  (void)AM;
#endif
  bool Changed = false;
  for (Observation &Obs : G)
    Changed |= mergeLocations(Obs);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
