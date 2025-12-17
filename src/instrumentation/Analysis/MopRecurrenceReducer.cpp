#include "MopRecurrenceReducer.h"
#include "../Utils/Logging.h"
#include "../Utils/Options.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include <array>
#include <optional>
#include <type_traits>

using namespace llvm;

#define DEBUG_TYPE "xsan-recurrence-reducer"

STATISTIC(NumRecurentChecks, "Number of recurent checks reduced.");

namespace __xsan {

struct Edge {
  const Instruction *Killing;
  const Instruction *Dead;
  const Instruction *From;
  // A edge could be blocked if ActiveMopAnalysis indidates that
  // there is a call with side effect between From and To.
  bool Blocked;

  Edge(const Instruction *Killing, const Instruction *Dead,
       const Instruction *From, bool Blocked)
      : Killing(Killing), Dead(Dead), From(From), Blocked(Blocked) {}
};

/*
  Vertex: a MOP
  Edge: if MOP2 is recurring for MOP1, then (MOP1, MOP2) is an edge
*/
class RecurringGraph {
public:
  struct Vertex {
    const Instruction *const Mop;
    SmallVector<const Vertex *, 16> Children;
    bool HasParent;

    using child_iterator = decltype(Children)::iterator;
    using const_child_iterator = decltype(Children)::const_iterator;

    Vertex(const Instruction *const Mop) : Mop(Mop), HasParent(false) {}
    void addChildren(Vertex &Child) {
      Children.push_back(&Child);
      Child.HasParent = true;
    }
  };

public:
  RecurringGraph(const SmallVector<const Instruction *, 16> &Vertices,
                 const SmallVectorImpl<Edge> &Edges)
      : Vertices(Vertices.begin(), Vertices.end()) {
    for (auto &V : this->Vertices) {
      // Mop2Vertex.try_emplace(V.Mop, &V);
      Mop2Vertex.insert({V.Mop, &V});
    }

    for (const auto &[Killing, Dead, From, Blocked] : Edges) {
      if (Blocked)
        continue;
      Vertex &KillingV = *Mop2Vertex[Killing];
      Vertex &DeadV = *Mop2Vertex[Dead];
      KillingV.addChildren(DeadV);
    }

    Vertex VirtualV(nullptr);
  }

  /// In directed graph, the dominating set of vertices is the set of vertices
  /// that can reach any vertex in the graph.
  /// Any entry of SCC with ZERO in-degree belongs to the dominating set.
  /// -->   Any vertex with ZERO in-degree belongs to the dominating set.
  ///     + Loop: 1. Add the vertex hasing not been traversed from any other
  ///                vertex into dominating set.
  ///             2. Travese from any vertex in dominating set.
  void fillDominatingSet(SmallVectorImpl<const Instruction *> &DomSet);

private:
  SmallVector<Vertex, 16> Vertices;
  SmallDenseMap<const Instruction *, Vertex *> Mop2Vertex;
};

} // namespace __xsan

namespace llvm {

template <> struct GraphTraits<__xsan::RecurringGraph::Vertex *> {
  using NodeRef = __xsan::RecurringGraph::Vertex *;
  using ChildIteratorType = std::remove_pointer<NodeRef>::type::child_iterator;

  static NodeRef getEntryNode(NodeRef Node) { return Node; }

  static ChildIteratorType child_begin(NodeRef Node) {
    return Node->Children.begin();
  }

  static ChildIteratorType child_end(NodeRef Node) {
    return Node->Children.end();
  }
};

template <> struct GraphTraits<const __xsan::RecurringGraph::Vertex *> {

  using NodeRef = const __xsan::RecurringGraph::Vertex *;
  using ChildIteratorType =
      std::remove_pointer<NodeRef>::type::const_child_iterator;

  static NodeRef getEntryNode(NodeRef Node) { return Node; }

  static ChildIteratorType child_begin(NodeRef Node) {
    return Node->Children.begin();
  }

  static ChildIteratorType child_end(NodeRef Node) {
    return Node->Children.end();
  }
};
} // namespace llvm

namespace __xsan {

static std::optional<TypeSize> getPointerSize(const Value *V,
                                              const DataLayout &DL,
                                              const TargetLibraryInfo &TLI,
                                              const Function *F) {
  uint64_t Size;
  ObjectSizeOpts Opts;
  Opts.NullIsUnknownSize = NullPointerIsDefined(F);

  if (getObjectSize(V, Size, DL, &TLI, Opts))
    return TypeSize::getFixed(Size);
  return std::nullopt;
}

/// Check if two instruction are masked stores that completely
/// overwrite one another. More specifically, \p KillingI has to
/// overwrite \p DeadI.
static bool isMaskedStoreOverwrite(const Instruction *KillingI,
                                   const Instruction *DeadI,
                                   BatchAAResults &AA) {
  const auto *KillingII = dyn_cast<IntrinsicInst>(KillingI);
  const auto *DeadII = dyn_cast<IntrinsicInst>(DeadI);
  if (KillingII == nullptr || DeadII == nullptr)
    return false;
  if (KillingII->getIntrinsicID() != Intrinsic::masked_store ||
      DeadII->getIntrinsicID() != Intrinsic::masked_store)
    return false;
  // Pointers.
  Value *KillingPtr = KillingII->getArgOperand(1)->stripPointerCasts();
  Value *DeadPtr = DeadII->getArgOperand(1)->stripPointerCasts();
  if (KillingPtr != DeadPtr && !AA.isMustAlias(KillingPtr, DeadPtr))
    return false;
  // Masks.
  // TODO: check that KillingII's mask is a superset of the DeadII's mask.
  if (KillingII->getArgOperand(3) != DeadII->getArgOperand(3))
    return false;
  return true;
}

static bool isCall(const Instruction *Inst, bool IgnoreNoSanitized = true) {
  // check call/invoke instruction
  if (((!isa<CallInst>(Inst) || isa<DbgInfoIntrinsic>(Inst)) &&
       isa<InvokeInst>(Inst))) {
    return false;
  }
  // check call decorated with non-sanitize metadata
  /// TODO: use other metatdata? nosanitize metadata might be used by users.
  if (Inst->hasMetadata(LLVMContext::MD_nosanitize))
    return false;

  return true;
}

/// Modified from DSEState in DeadStoreElimination.cpp
struct MOPState {
  Function &F;
  AliasAnalysis &AA;
  EarliestEscapeInfo EI;

  /// The single BatchAA instance that is used to cache AA queries. It will
  /// not be invalidated over the whole run. This is safe, because:
  /// 1. Only memory writes are removed, so the alias cache for memory
  ///    locations remains valid.
  /// 2. No new instructions are added (only instructions removed), so cached
  ///    information for a deleted value cannot be accessed by a re-used new
  ///    value pointer.
  BatchAAResults BatchAA;

  // MemorySSA &MSSA;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  const TargetLibraryInfo &TLI;
  const DataLayout &DL;
  const LoopInfo &LI;

  // Whether the function contains any irreducible control flow, useful for
  // being accurately able to detect loops.
  bool ContainsIrreducibleLoops;

  // Values that are only used with assumes. Used to refine pointer escape
  // analysis.
  SmallPtrSet<const Value *, 32> EphValues;

  // Class contains self-reference, make sure it's not copied/moved.
  MOPState(const MOPState &) = delete;
  MOPState &operator=(const MOPState &) = delete;

  MOPState(Function &F, AliasAnalysis &AA, DominatorTree &DT,
           PostDominatorTree &PDT, const TargetLibraryInfo &TLI,
           const LoopInfo &LI)
      : F(F), AA(AA), EI(DT, LI, EphValues), BatchAA(AA, &EI), DT(DT), PDT(PDT),
        TLI(TLI), DL(F.getParent()->getDataLayout()), LI(LI) {

    // Collect whether there is any irreducible control flow in the function.
    ContainsIrreducibleLoops = mayContainIrreducibleControl(F, &LI);
  }

  MOPState(Function &F, FunctionAnalysisManager &FAM)
      : MOPState(F, FAM.getResult<AAManager>(F),
                 FAM.getResult<DominatorTreeAnalysis>(F),
                 FAM.getResult<PostDominatorTreeAnalysis>(F),
                 FAM.getResult<TargetLibraryAnalysis>(F),
                 FAM.getResult<LoopAnalysis>(F)) {}

  /// Returns true if a dependency between \p Current and \p KillingDef is
  /// guaranteed to be loop invariant for the loops that they are in. Either
  /// because they are known to be in the same block, in the same loop level or
  /// by guaranteeing that \p CurrentLoc only references a single MemoryLocation
  /// during execution of the containing function.
  bool isGuaranteedLoopIndependent(const Instruction *Current,
                                   const Instruction *KillingDef,
                                   const MemoryLocation &CurrentLoc) {
    // If the dependency is within the same block or loop level (being careful
    // of irreducible loops), we know that AA will return a valid result for the
    // memory dependency. (Both at the function level, outside of any loop,
    // would also be valid but we currently disable that to limit compile time).
    if (Current->getParent() == KillingDef->getParent())
      return true;
    const Loop *CurrentLI = LI.getLoopFor(Current->getParent());
    if (!ContainsIrreducibleLoops && CurrentLI &&
        CurrentLI == LI.getLoopFor(KillingDef->getParent()))
      return true;
    // Otherwise check the memory location is invariant to any loops.
    return isGuaranteedLoopInvariant(CurrentLoc.Ptr);
  }

  /// Returns true if \p Ptr is guaranteed to be loop invariant for any possible
  /// loop. In particular, this guarantees that it only references a single
  /// MemoryLocation during execution of the containing function.
  bool isGuaranteedLoopInvariant(const Value *Ptr) {
    Ptr = Ptr->stripPointerCasts();
    if (auto *GEP = dyn_cast<GEPOperator>(Ptr))
      if (GEP->hasAllConstantIndices())
        Ptr = GEP->getPointerOperand()->stripPointerCasts();

    if (auto *I = dyn_cast<Instruction>(Ptr)) {
      return I->getParent()->isEntryBlock() ||
             (!ContainsIrreducibleLoops && !LI.getLoopFor(I->getParent()));
    }
    return true;
  }

  LocationSize strengthenLocationSize(const Instruction *I,
                                      LocationSize Size) const {
    if (auto *CB = dyn_cast<CallBase>(I)) {
      LibFunc F;
      if (TLI.getLibFunc(*CB, F) && TLI.has(F) &&
          (F == LibFunc_memset_chk || F == LibFunc_memcpy_chk)) {
        // Use the precise location size specified by the 3rd argument
        // for determining KillingI overwrites DeadLoc if it is a memset_chk
        // instruction. memset_chk will write either the amount specified as 3rd
        // argument or the function will immediately abort and exit the program.
        // NOTE: AA may determine NoAlias if it can prove that the access size
        // is larger than the allocation size due to that being UB. To avoid
        // returning potentially invalid NoAlias results by AA, limit the use of
        // the precise location size to isOverwrite.
        if (const auto *Len = dyn_cast<ConstantInt>(CB->getArgOperand(2)))
          return LocationSize::precise(Len->getZExtValue());
      }
    }
    return Size;
  }

  /// Return 'true' if a store to the 'KillingLoc' location (by \p
  /// KillingI instruction) completely overwrites a store to the 'DeadLoc'
  /// location (by \p DeadI instruction).
  /// Return false if \p KillingI does not completely overwrite
  /// \p DeadI, but they both write to the same underlying object. In that
  /// case, use isPartialOverwrite to check if \p KillingI partially overwrites
  /// \p DeadI. Returns 'OR_None' if \p KillingI is known to not overwrite the
  /// \p DeadI. Returns 'false' if nothing can be determined.
  bool isAccessRangeContains(const Instruction *KillingI,
                             const Instruction *DeadI,
                             const MemoryLocation &KillingLoc,
                             const MemoryLocation &DeadLoc, int64_t &KillingOff,
                             int64_t &DeadOff) {
    // AliasAnalysis does not always account for loops. Limit overwrite checks
    // to dependencies for which we can guarantee they are independent of any
    // loops they are in.
    if (!isGuaranteedLoopIndependent(DeadI, KillingI, DeadLoc))
      return false;

    LocationSize KillingLocSize =
        strengthenLocationSize(KillingI, KillingLoc.Size);
    const Value *DeadPtr = DeadLoc.Ptr->stripPointerCasts();
    const Value *KillingPtr = KillingLoc.Ptr->stripPointerCasts();
    const Value *DeadUndObj = getUnderlyingObject(DeadPtr);
    const Value *KillingUndObj = getUnderlyingObject(KillingPtr);

    // Check whether the killing store overwrites the whole object, in which
    // case the size/offset of the dead store does not matter.
    if (DeadUndObj == KillingUndObj && KillingLocSize.isPrecise() &&
        isIdentifiedObject(KillingUndObj)) {
      std::optional<TypeSize> KillingUndObjSize =
          getPointerSize(KillingUndObj, DL, TLI, &F);
      if (KillingUndObjSize && *KillingUndObjSize == KillingLocSize.getValue())
        return true;
    }

    // FIXME: Vet that this works for size upper-bounds. Seems unlikely that
    // we'll get imprecise values here, though (except for unknown sizes).
    if (!KillingLoc.Size.isPrecise() || !DeadLoc.Size.isPrecise()) {
      // In case no constant size is known, try to an IR values for the number
      // of bytes written and check if they match.
      const auto *KillingMemI = dyn_cast<MemIntrinsic>(KillingI);
      const auto *DeadMemI = dyn_cast<MemIntrinsic>(DeadI);
      if (KillingMemI && DeadMemI) {
        const Value *KillingV = KillingMemI->getLength();
        const Value *DeadV = DeadMemI->getLength();
        if (KillingV == DeadV && BatchAA.isMustAlias(DeadLoc, KillingLoc))
          return true;
      }

      // Masked stores have imprecise locations, but we can reason about them
      // to some extent.
      return isMaskedStoreOverwrite(KillingI, DeadI, BatchAA);
    }

    const uint64_t KillingSize = KillingLoc.Size.getValue();
    const uint64_t DeadSize = DeadLoc.Size.getValue();

    // Query the alias information
    AliasResult AAR = BatchAA.alias(KillingLoc, DeadLoc);

    // If the start pointers are the same, we just have to compare sizes to see
    // if the killing store was larger than the dead store.
    if (AAR == AliasResult::MustAlias) {
      // Make sure that the KillingSize size is >= the DeadSize size.
      if (KillingSize >= DeadSize)
        return true;
    }

    // If we hit a partial alias we may have a full overwrite
    if (AAR == AliasResult::PartialAlias && AAR.hasOffset()) {
      int32_t Off = AAR.getOffset();
      if (Off >= 0 && (uint64_t)Off + DeadSize <= KillingSize)
        return true;
    }

    // If we can't resolve the same pointers to the same object, then we can't
    // analyze them at all.
    if (DeadUndObj != KillingUndObj) {
      return false;
    }

    // Okay, we have stores to two completely different pointers.  Try to
    // decompose the pointer into a "base + constant_offset" form.  If the base
    // pointers are equal, then we can reason about the two stores.
    DeadOff = 0;
    KillingOff = 0;
    const Value *DeadBasePtr =
        GetPointerBaseWithConstantOffset(DeadPtr, DeadOff, DL);
    const Value *KillingBasePtr =
        GetPointerBaseWithConstantOffset(KillingPtr, KillingOff, DL);

    // If the base pointers still differ, we have two completely different
    // stores.
    if (DeadBasePtr != KillingBasePtr)
      return false;

    // The killing access completely overlaps the dead store if and only if
    // both start and end of the dead one is "inside" the killing one:
    //    |<->|--dead--|<->|
    //    |-----killing------|
    // Accesses may overlap if and only if start of one of them is "inside"
    // another one:
    //    |<->|--dead--|<-------->|
    //    |-------killing--------|
    //           OR
    //    |-------dead-------|
    //    |<->|---killing---|<----->|
    //
    // We have to be careful here as *Off is signed while *.Size is unsigned.

    // Check if the dead access starts "not before" the killing one.
    if (DeadOff >= KillingOff) {
      // If the dead access ends "not after" the killing access then the
      // dead one is completely overwritten by the killing one.
      if (uint64_t(DeadOff - KillingOff) + DeadSize <= KillingSize)
        return true;
    }

    // Can reach here only if accesses are known not to overlap.
    return false;
  }

  /*
  MOP = (range, isWrite) defines a memory operation.
  For any TWO MOPs {MOP1 = (range1, isWrite1), MOP2 = (range2, isWrite2)} that
  satisfy the following conditions, the corresponding check for MOP2 is
  duplicated.
    - Contains(range1, range2), considering aliases.
    - MOP1 dom MOP2 || MOP1 pdom MOP2
    - (optional) isWrite1 || (isWrite1 == isWrite2)
  Return the FromI (FYI, from inst -> to inst) if recurring, otherwise, return
  nullptr.
  */
  const Instruction *isMopCheckRecurring(const MemoryOperation &KillingMop,
                                         const MemoryOperation &DeadMop,
                                         bool WriteSensitive = true) {
    const Instruction *KillingI = KillingMop.Inst;
    const Instruction *DeadI = DeadMop.Inst;
    if (!isInterestingMop(*KillingI, true) || !isInterestingMop(*DeadI, true)) {
      return nullptr;
    }

    // (optional) isWrite1 || (isWrite1 == isWrite2)
    if (WriteSensitive && !KillingMop.isWrite() &&
        KillingMop.isWrite() != DeadMop.isWrite()) {
      return nullptr;
    }

    const Instruction *FromI = nullptr;
    /// MOP1 dom MOP2 || MOP1 pdom MOP2
    if (DT.dominates(KillingI, DeadI)) {
      FromI = KillingI;
    } else if (PDT.dominates(KillingI, DeadI)) {
      FromI = DeadI;
    } else {
      return nullptr;
    }

    // Contains(range1, range2), considering aliases.
    int64_t KillingOff, DeadOff;
    if (!isAccessRangeContains(KillingMop.Inst, DeadMop.Inst, KillingMop.Loc,
                               DeadMop.Loc, KillingOff, DeadOff)) {
      return nullptr;
    }

    return FromI;
  }
};

struct MOPGroup {
  [[maybe_unused]] const Value *BasePtr;
  SmallVector<const Instruction *, 4> Group;

  MOPGroup(const Value *BasePtr, SmallVector<const Instruction *, 4> &&Group)
      : BasePtr(BasePtr), Group(std::move(Group)) {}
};
using MOPGroupVec = SmallVector<MOPGroup, 4>;

/// @brief Group MOPs by the base pointer.
/// @details We consider the MustAlias/PartialAlias pairs sharing the same base
/// pointer. Thus, without considering aliases, we groups memory operations by
/// their base pointer to optimize recurrence analysis. Only returns groups with
/// 2 or more MOPs, as single-MOP groups cannot have recurring checks. This
/// enables O(N^2/m) complexity instead of O(N^2) where m is the number of
/// groups.
/// @param Insts The memory operations to group.
/// @param DL The data layout.
/// @return The groups of memory operations.
static MOPGroupVec groupMOPsByBasePtr(ArrayRef<const Instruction *> Insts,
                                      const DataLayout &DL) {

  using MOPGroupMap =
      DenseMap<const Value *, SmallVector<const Instruction *, 4>>;

  MOPGroupMap BasePtr2MOPs;
  size_t GroupSize = 0;
  for (const Instruction *I : Insts) {
    MemoryLocation Loc = MemoryLocation::get(I);
    const Value *Ptr = Loc.Ptr->stripPointerCasts();
    /// ASSUME: all MustAlias/PartialAlias pairs must share the same base
    /// pointer. Therefore, we are able to group them by the base pointer
    /// firstly without considering aliases. This assumption holds at
    /// BasicAliasAnalysis.
    const Value *BasePtr = getUnderlyingObject(Ptr);
    BasePtr2MOPs[BasePtr].push_back(I);
    if (BasePtr2MOPs[BasePtr].size() == 2)
      GroupSize++;
  }
  MOPGroupVec MOPGrps;
  MOPGrps.reserve(GroupSize);
  for (auto &[BasePtr, MOPs] : BasePtr2MOPs)
    if (MOPs.size() >= 2)
      MOPGrps.emplace_back(BasePtr, std::move(MOPs));
  return MOPGrps;
}

void RecurringGraph::fillDominatingSet(
    SmallVectorImpl<const Instruction *> &DomSet) {
  // Because there could be several/many MOPs, remember which
  // MOPs is recurring to others.
  df_iterator_default_set<const Vertex *, 16> Visited;
  for (const Vertex &V : Vertices) {
    if (V.HasParent) {
      continue;
    }
    DomSet.push_back(V.Mop);

    for (const Vertex *V2 : depth_first_ext(&V, Visited)) {
    }
  }

  for (const Vertex &V : Vertices) {
    if (!V.HasParent || Visited.contains(&V)) {
      continue;
    }
    DomSet.push_back(V.Mop);

    for (const Vertex *V2 : depth_first_ext(&V, Visited)) {
    }
  }
}

MopRecurrenceReducer::MopRecurrenceReducer(Function &F,
                                           FunctionAnalysisManager &FAM)
    : F(F), FAM(FAM) {}

/*
Distill any pair of MOPs satisfying the following 4 cases:
  - Contains(range1, range2), considering aliases.
  - MOP1 dom MOP2 || MOP1 pdom MOP2
  - (optional) isWrite1 || (isWrite1 == isWrite2)
  - no-clobbering-calls between MOP1 and MOP2
This is the dominating set problem in directed graph.
*/
SmallVector<const Instruction *, 16>
MopRecurrenceReducer::distillRecurringChecks(
    ArrayRef<const Instruction *> Insts, bool IsTsan, bool IgnoreCalls) {

  if (Insts.size() < 2)
    return SmallVector<const Instruction *, 16>(Insts.begin(), Insts.end());

  MOPGroupVec MOPGrps =
      groupMOPsByBasePtr(Insts, F.getParent()->getDataLayout());
  if (MOPGrps.empty())
    return SmallVector<const Instruction *, 16>(Insts.begin(), Insts.end());

  SmallVector<Edge, 16> Edges;
  SmallSetVector<const Instruction *, 16> CandidatesSet;

  MOPState State(F, FAM);

  bool WriteSensitive = IsTsan;

  /*
    - Contains(range1, range2), considering aliases.
    - MOP1 dom MOP2 || MOP1 pdom MOP2
    - (optional) isWrite1 || (isWrite1 == isWrite2)
  */
  // Use the grouping strategy to optimize the O(N^2) traversal into O(N^2 / m)
  for (auto &Group : MOPGrps) {
    // The worst case is still O(N^2) if we only have one major group.
    for (const Instruction *KillingI : Group.Group)
      for (const Instruction *DeadI : Group.Group) {
        if (KillingI == DeadI)
          continue;

        // /// Volatile MOP's check could not have been eliminated.
        // /// However, TSan ClDistinguishVolatile is always false, that means
        // /// TSan does not treat volatile MOP's specially.
        // if (DeadI->isVolatile())
        //   continue;

        const Instruction *FromI =
            State.isMopCheckRecurring(KillingI, DeadI, WriteSensitive);
        if (!FromI)
          continue;
        Edges.emplace_back(KillingI, DeadI, FromI, false);

        CandidatesSet.insert(KillingI);
        CandidatesSet.insert(DeadI);
      }
  }

  /// Initialized with Insts with neither Killing nor Dead Mops.
  SmallVector<const Instruction *, 16> DistilledMops(make_filter_range(
      Insts, [&](const Instruction *I) { return !CandidatesSet.contains(I); }));

  /// CandidatesSet is moved to Candidates.
  SmallVector<const Instruction *, 16> Candidates = CandidatesSet.takeVector();

  /*
    - no-clobbering-calls between MOP1 and MOP2
    (For TSan, we also pay extra attention to atomic instructions with
     RELEASE/ACQUIRE memory ordering)
  */
  if (!IgnoreCalls) {
    ActiveMopAnalysis Analysis(F, Candidates, IsTsan);
    for (auto &[Killing, Dead, From, Blocked] : Edges) {
      const bool IsToDead = From == Killing;
      const Instruction *To = IsToDead ? Dead : Killing;
      // If there is a call between any path from FromI to ToI, then the
      // corresponding MOPs are not recurring.
      Blocked = !Analysis.isOneMopActiveToAnother(From, To, IsToDead);
    }
  }

  RecurringGraph RGraph(Candidates, Edges);

  /* Dominating set problem solving */
  RGraph.fillDominatingSet(DistilledMops);

  NumRecurentChecks += Insts.size() - DistilledMops.size();

  // Collect debug information
  if (options::ClDebug) {
    Log.setFunction(F.getName());
    Log.addLog(IsTsan ? "[ReccOpt] [TSan] Reducing Recurr. Checks"
                      : "[ReccOpt] [ASan] Reducing Recurr. Checks",
               Insts.size(), DistilledMops.size());
  }
  return DistilledMops;
}
} // namespace __xsan