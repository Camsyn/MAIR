#include "MAIR/Transforms/LocationMergePass.h"
#include "MAIR/Analysis/LoopAnalysis.h"
#include "MAIR/Analysis/MAIRStatAnalysis.h"
#include "MAIR/MAIR.h"
#include "MAIR/MAIRStatLog.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Module.h"

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

/// The distances of the slices that, with \p Current at distance 0, interleave
/// into one periodic region of m = |Stride| / g slices for some gap g, with as
/// many slices as possible. \p Slices holds the distances of the candidates.
SmallVector<int64_t, 4> findInterleavedGroup(ArrayRef<int64_t> Slices,
                                             uint64_t AbsStride) {
  auto Has = [&](int64_t D) { return D == 0 || is_contained(Slices, D); };
  SmallVector<uint64_t, 4> Gaps;
  for (int64_t D : Slices)
    Gaps.push_back(D < 0 ? -static_cast<uint64_t>(D) : D);
  llvm::sort(Gaps);
  for (uint64_t Gap : Gaps) {
    const uint64_t M = AbsStride / Gap;
    if (AbsStride % Gap != 0 || M > Slices.size() + 1)
      continue;
    // Current is slice -Lowest of the group starting at distance Lowest * g.
    const int64_t G = Gap;
    for (int64_t Lowest = 1 - static_cast<int64_t>(M); Lowest <= 0; ++Lowest) {
      SmallVector<int64_t, 4> Group;
      for (int64_t K = Lowest; K < Lowest + static_cast<int64_t>(M); ++K)
        if (K != 0 && Has(K * G))
          Group.push_back(K * G);
      if (Group.size() + 1 == M)
        return Group;
    }
  }
  return {};
}

/// Merge each group of periodic observations that are the interleaved slices
/// of one periodic region (PeriodicRegion::mergeInterleaved). A group is found
/// from its member listed first, which absorbs the others, so no observation
/// at or before Current is erased.
bool mergeInterleavedRange(ProgramPoint &PP, ObservationIterator Begin,
                           ObservationIterator End) {
  bool Changed = false;
  for (ObservationIterator Current = Begin; Current != End;) {
    const PeriodicRegion *Region = Current->getPeriodicRegion();
    const Instruction *Provenance = Current->getInstruction();
    if (!Region || !Provenance ||
        Region->getStride().getKind() != MAIRScalarExpr::Kind::Constant) {
      ++Current;
      continue;
    }
    const DataLayout &DL = Provenance->getModule()->getDataLayout();
    // Candidates in list order, so that their sources are appended in order.
    SmallVector<int64_t, 4> Distances;
    SmallVector<ObservationIterator, 4> Candidates;
    for (ObservationIterator Next = std::next(Current); Next != End; ++Next)
      if (const PeriodicRegion *Other = Next->getPeriodicRegion())
        if (std::optional<int64_t> D =
                PeriodicRegion::getInterleaveDistance(*Region, *Other, DL)) {
          Distances.push_back(*D);
          Candidates.push_back(Next);
        }
    if (Distances.empty()) {
      ++Current;
      continue;
    }
    SmallVector<int64_t, 4> Group = findInterleavedGroup(
        Distances, Region->getStride().getConstant().abs().getZExtValue());
    // One candidate per distance; equal regions were merged before.
    SmallVector<int64_t, 4> Taken;
    SmallVector<ObservationIterator, 4> Members;
    SmallVector<const Observation *, 4> Others;
    for (auto [D, It] : zip(Distances, Candidates))
      if (is_contained(Group, D) && !is_contained(Taken, D)) {
        Taken.push_back(D);
        Members.push_back(It);
        Others.push_back(&*It);
      }
    if (Group.empty() || !Current->mergeInterleaved(Others)) {
      ++Current;
      continue;
    }
    for (ObservationIterator It : Members) {
      PP.erase(It);
      ++NumMergedObservations;
      MAIRStatScope::add(MAIRStatField::Merged);
    }
    Changed = true;
    // The merged region has a smaller stride and may interleave again.
  }
  return Changed;
}

bool mergeLocations(ProgramPoint &PP, ScalarEvolution *SE) {
  PP.sort(compareForLocationMerge);

  bool Changed = false;
  for (ObservationIterator Begin = PP.begin(); Begin != PP.end();) {
    if (!Begin->isMergeable()) {
      ++Begin;
      continue;
    }

    ObservationIterator End = std::next(Begin);
    while (End != PP.end() && hasSameMergeClass(*Begin, *End))
      ++End;

    // Symbolic ranges and periodic regions have no order to sort by, so they
    // try every pair.
    if (Begin->hasConcreteRange())
      Changed |= mergeConcreteRange(PP, Begin, End, SE);
    else
      Changed |= mergeNonConcreteRange(PP, Begin, End, SE);
    // Slices that leave gaps do not merge pairwise; they may interleave.
    if (getMergeClass(*Begin) == MergeClass::Periodic)
      Changed |= mergeInterleavedRange(PP, Begin, End);
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
