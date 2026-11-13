#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRStatLog.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ScalarEvolution.h"

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

// Constants in ascending order, then symbols, then unknown. Symbols have no
// run-independent order, so they compare equivalent and the stable sort keeps
// them in list order.
bool compareAbstractValue(const AbstractValue &LHS, const AbstractValue &RHS) {
  if (LHS.isConstant() != RHS.isConstant())
    return LHS.isConstant();
  if (LHS.isConstant())
    return LHS.getConstant() < RHS.getConstant();
  return LHS.isSymbol() && RHS.isUnknown();
}

bool compareForLocationMerge(const Observation &LHS, const Observation &RHS) {
  const MemoryObject &LObject = LHS.getMemoryObject();
  const MemoryObject &RObject = RHS.getMemoryObject();
  if (&LObject != &RObject)
    return LObject.getID() < RObject.getID();
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
  // Only constant offsets order the sizes behind them. Ordering sizes behind
  // equivalent symbolic offsets would not be a strict weak ordering.
  if (!LBounds->getOffset().isConstant() ||
      !RBounds->getOffset().isConstant() ||
      LBounds->getOffset() != RBounds->getOffset())
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
                        ObservationIterator End, ScalarEvolution *SE) {
  if (Begin == End)
    return false;

  bool Changed = false;
  ObservationIterator Current = Begin;
  for (ObservationIterator Next = std::next(Begin); Next != End;) {
    if (!Current->mergeWith(*Next, SE)) {
      Current = Next++;
      continue;
    }
    Next = PP.erase(Next);
    Changed = true;
    ++NumMergedObservations;
    MAIRStatScope::add(MAIRStatField::Merged);
  }
  return Changed;
}

bool mergeNonConcreteRange(ProgramPoint &PP, ObservationIterator Begin,
                           ObservationIterator End, ScalarEvolution *SE) {
  bool Changed = false;

  for (ObservationIterator Current = Begin; Current != End; ++Current) {
    for (ObservationIterator Next = std::next(Current); Next != End;) {
      if (!Current->mergeWith(*Next, SE)) {
        ++Next;
        continue;
      }
      PP.erase(Next);
      Next = std::next(Current);
      Changed = true;
      ++NumMergedObservations;
      MAIRStatScope::add(MAIRStatField::Merged);
    }
  }
  return Changed;
}

bool mergeLocations(ProgramPoint &PP, ScalarEvolution *SE) {
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
      Changed |= mergeConcreteRange(PP, Begin, End, SE);
    else
      Changed |= mergeNonConcreteRange(PP, Begin, End, SE);
    Begin = End;
  }
  return Changed;
}

} // namespace

PreservedAnalyses LocationMergePass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
  MAIRStatScope Scope("locationmerge", Ordinal, G, NumInputObservations, [&] {
    return MAIRStat::compute(G).getNumObservations();
  });
  ScalarEvolution *SE = nullptr;
  if (AM.isPassRegistered<LoopAnalysis>()) {
    MAIRLoopInfo &Loops = AM.getResult<LoopAnalysis>(G);
    if (Loops.isAvailable())
      SE = &Loops.getSE();
  }
  bool Changed = false;
  for (ProgramPoint &PP : G)
    Changed |= mergeLocations(PP, SE);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace mair
