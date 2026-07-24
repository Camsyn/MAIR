#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/Statistic.h"

#include <iterator>

using namespace llvm;

#define DEBUG_TYPE "mair-locationmerge"

STATISTIC(NumMergedAccesses, "Number of accesses merged by location merging");
STATISTIC(NumInputAccesses,
          "Number of memory accesses entering location merging");

namespace mair {

namespace {

using AccessIterator = Observation::iterator;

bool hasSameMemoryObject(const MemoryAccess &LHS, const MemoryAccess &RHS) {
  return &LHS.getMemoryObject() == &RHS.getMemoryObject();
}

bool mergeConcreteRange(Observation &Obs, AccessIterator Begin,
                        AccessIterator End) {
  bool Changed = false;
  if (Begin == End)
    return false;

  AccessIterator Current = Begin;
  AccessIterator Next = std::next(Current);
  while (Next != End) {
    if (!Current->mergeWith(*Next)) {
      Current = Next;
      Next = std::next(Current);
      continue;
    }
    Next = Obs.erase(Next);
    Changed = true;
    ++NumMergedAccesses;
  }
  return Changed;
}

bool mergeNonConcreteRange(Observation &Obs, AccessIterator Begin,
                           AccessIterator End) {
  bool Changed = false;

  for (AccessIterator Current = Begin; Current != End; ++Current) {
    for (AccessIterator Next = std::next(Current); Next != End;) {
      if (!Current->mergeWith(*Next)) {
        ++Next;
        continue;
      }
      Obs.erase(Next);
      Next = std::next(Current);
      Changed = true;
      ++NumMergedAccesses;
    }
  }
  return Changed;
}

bool mergeLocations(Observation &Obs) {
  Obs.sort(MemoryAccess::compareByLocation);

  bool Changed = false;
  for (AccessIterator ObjectBegin = Obs.begin(); ObjectBegin != Obs.end();) {
    AccessIterator ObjectEnd = std::next(ObjectBegin);
    while (ObjectEnd != Obs.end() &&
           hasSameMemoryObject(*ObjectBegin, *ObjectEnd))
      ++ObjectEnd;

    AccessIterator ConcreteEnd = ObjectBegin;
    while (ConcreteEnd != ObjectEnd && !ConcreteEnd->isPeriodic() &&
           ConcreteEnd->hasConcreteRange())
      ++ConcreteEnd;

    Changed |= mergeConcreteRange(Obs, ObjectBegin, ConcreteEnd);

    AccessIterator NonConcreteEnd = ConcreteEnd;
    while (NonConcreteEnd != ObjectEnd && !NonConcreteEnd->isPeriodic())
      ++NonConcreteEnd;
    Changed |= mergeNonConcreteRange(Obs, ConcreteEnd, NonConcreteEnd);

    ObjectBegin = ObjectEnd;
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
