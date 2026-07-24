#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/Statistic.h"

#include <functional>
#include <iterator>

using namespace llvm;

#define DEBUG_TYPE "mair-locationmerge"

STATISTIC(NumMergedAccesses, "Number of accesses merged by location merging");
STATISTIC(NumInputAccesses,
          "Number of memory accesses entering location merging");

namespace mair {

namespace {

using AccessIterator = Observation::iterator;

enum class MergeClass : uint8_t {
  ConcreteContiguous,
  NonConcreteContiguous,
  Masked,
  Periodic,
};

MergeClass getMergeClass(const MemoryAccess &Access) {
  if (Access.hasConcreteRange())
    return MergeClass::ConcreteContiguous;
  if (Access.isContiguous())
    return MergeClass::NonConcreteContiguous;
  if (Access.getMaskedRegion())
    return MergeClass::Masked;
  return MergeClass::Periodic;
}

const AbstractAccess *getLocationBounds(const MemoryAccess &Access) {
  if (const AbstractAccess *Region = Access.getContiguousRegion())
    return Region;
  if (const MaskedRegion *Region = Access.getMaskedRegion())
    return &Region->getBounds();
  return nullptr;
}

bool compareAbstractValue(const AbstractValue &LHS, const AbstractValue &RHS) {
  if (LHS.isConstant() != RHS.isConstant())
    return LHS.isConstant();
  if (LHS.isConstant())
    return LHS.getConstant() < RHS.getConstant();
  if (LHS.isSymbol() != RHS.isSymbol())
    return LHS.isSymbol();
  if (LHS.isSymbol())
    return std::less<const Value *>()(LHS.getSymbol(), RHS.getSymbol());
  return false;
}

bool compareForLocationMerge(const MemoryAccess &LHS, const MemoryAccess &RHS) {
  const MemoryObject *LObject = LHS.getBaseUse().get();
  const MemoryObject *RObject = RHS.getBaseUse().get();
  if (LObject != RObject)
    return std::less<const MemoryObject *>()(LObject, RObject);
  if (LHS.isMergeable() != RHS.isMergeable())
    return LHS.isMergeable();
  if (LHS.getLevel() != RHS.getLevel())
    return LHS.getLevel() < RHS.getLevel();

  MergeClass LClass = getMergeClass(LHS);
  MergeClass RClass = getMergeClass(RHS);
  if (LClass != RClass)
    return LClass < RClass;

  const AbstractAccess *LBounds = getLocationBounds(LHS);
  const AbstractAccess *RBounds = getLocationBounds(RHS);
  if (!LBounds || !RBounds)
    return false;
  if (LBounds->getOffset() != RBounds->getOffset())
    return compareAbstractValue(LBounds->getOffset(), RBounds->getOffset());
  if (LBounds->getSize() == RBounds->getSize())
    return false;
  if (LBounds->getSize().isConstant() && RBounds->getSize().isConstant())
    return LBounds->getSize().getConstant() > RBounds->getSize().getConstant();
  return compareAbstractValue(LBounds->getSize(), RBounds->getSize());
}

bool hasSameMergeClass(const MemoryAccess &LHS, const MemoryAccess &RHS) {
  return LHS.getBaseUse().get() == RHS.getBaseUse().get() &&
         LHS.isMergeable() && RHS.isMergeable() &&
         LHS.getLevel() == RHS.getLevel() &&
         getMergeClass(LHS) == getMergeClass(RHS);
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
  Obs.sort(compareForLocationMerge);

  bool Changed = false;
  for (AccessIterator Begin = Obs.begin(); Begin != Obs.end();) {
    if (!Begin->isMergeable() || Begin->isPeriodic()) {
      ++Begin;
      continue;
    }

    AccessIterator End = std::next(Begin);
    while (End != Obs.end() && hasSameMergeClass(*Begin, *End))
      ++End;

    if (Begin->hasConcreteRange())
      Changed |= mergeConcreteRange(Obs, Begin, End);
    else
      Changed |= mergeNonConcreteRange(Obs, Begin, End);
    Begin = End;
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
