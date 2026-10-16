#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"

#include "llvm/ADT/Statistic.h"

#include <functional>
#include <iterator>

using namespace llvm;

#define DEBUG_TYPE "mair-locationmerge"

STATISTIC(NumMergedObservations,
          "Number of observations merged by location merging");
STATISTIC(NumInputObservations,
          "Number of observations entering location merging");

namespace mair {

namespace {

using ObservationIterator = ProgramPoint::iterator;

enum class MergeClass : uint8_t {
  ConcreteContiguous,
  NonConcreteContiguous,
  Masked,
  Periodic,
};

MergeClass getMergeClass(const Observation &Obs) {
  if (Obs.hasConcreteRange())
    return MergeClass::ConcreteContiguous;
  if (Obs.isContiguous())
    return MergeClass::NonConcreteContiguous;
  if (Obs.getMaskedRegion())
    return MergeClass::Masked;
  return MergeClass::Periodic;
}

const AbstractAccess *getLocationBounds(const Observation &Obs) {
  if (const AbstractAccess *Region = Obs.getContiguousRegion())
    return Region;
  if (const MaskedRegion *Region = Obs.getMaskedRegion())
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

bool compareForLocationMerge(const Observation &LHS, const Observation &RHS) {
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

bool hasSameMergeClass(const Observation &LHS, const Observation &RHS) {
  return LHS.getBaseUse().get() == RHS.getBaseUse().get() &&
         LHS.isMergeable() && RHS.isMergeable() &&
         LHS.getLevel() == RHS.getLevel() &&
         getMergeClass(LHS) == getMergeClass(RHS);
}

bool mergeConcreteRange(ProgramPoint &PP, ObservationIterator Begin,
                        ObservationIterator End) {
  bool Changed = false;
  if (Begin == End)
    return false;

  ObservationIterator Current = Begin;
  ObservationIterator Next = std::next(Current);
  while (Next != End) {
    if (!Current->mergeWith(*Next)) {
      Current = Next;
      Next = std::next(Current);
      continue;
    }
    Next = PP.erase(Next);
    Changed = true;
    ++NumMergedObservations;
  }
  return Changed;
}

bool mergeNonConcreteRange(ProgramPoint &PP, ObservationIterator Begin,
                           ObservationIterator End) {
  bool Changed = false;

  for (ObservationIterator Current = Begin; Current != End; ++Current) {
    for (ObservationIterator Next = std::next(Current); Next != End;) {
      if (!Current->mergeWith(*Next)) {
        ++Next;
        continue;
      }
      PP.erase(Next);
      Next = std::next(Current);
      Changed = true;
      ++NumMergedObservations;
    }
  }
  return Changed;
}

bool mergeLocations(ProgramPoint &PP) {
  PP.sort(compareForLocationMerge);

  bool Changed = false;
  for (ObservationIterator Begin = PP.begin(); Begin != PP.end();) {
    if (!Begin->isMergeable() || Begin->isPeriodic()) {
      ++Begin;
      continue;
    }

    ObservationIterator End = std::next(Begin);
    while (End != PP.end() && hasSameMergeClass(*Begin, *End))
      ++End;

    if (Begin->hasConcreteRange())
      Changed |= mergeConcreteRange(PP, Begin, End);
    else
      Changed |= mergeNonConcreteRange(PP, Begin, End);
    Begin = End;
  }
  return Changed;
}

} // namespace

PreservedAnalyses LocationMergePass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
#if LLVM_ENABLE_STATS
  NumInputObservations += MAIRStat::compute(G).getNumObservations();
#else
  (void)AM;
#endif
  bool Changed = false;
  for (ProgramPoint &PP : G)
    Changed |= mergeLocations(PP);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
