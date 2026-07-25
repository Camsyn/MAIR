#include "MAIR/MAIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

using namespace llvm;

namespace mair {

namespace {

struct MAIRVerifierSupport {
  raw_ostream *OS = nullptr;
  bool Broken = false;

  explicit MAIRVerifierSupport(raw_ostream *Out) : OS(Out) {}

  void Write(const Observation *Obs) {
    if (!Obs)
      return;
    Write(*Obs);
  }

  void Write(const Observation &Obs) {
    if (!OS)
      return;
    *OS << "  observation: ";
    Obs.print(*OS, true);
    *OS << "\n";
  }

  void Write(const MemoryAccess &Access) {
    if (!OS)
      return;
    *OS << "  access: ";
    Access.print(*OS, true);
    *OS << "\n";
  }

  void Write(const Instruction *I) {
    if (!OS || !I)
      return;
    *OS << "  instruction: ";
    I->print(*OS);
    *OS << "\n";
  }

  void Write(const Instruction &I) { Write(&I); }

  void Write(const BasicBlock *BB) {
    if (!OS || !BB)
      return;
    *OS << "  basic block: ";
    BB->printAsOperand(*OS, false);
    *OS << "\n";
  }

  void Write(const Value *V) {
    if (!OS || !V)
      return;
    *OS << "  value: ";
    V->printAsOperand(*OS, false);
    *OS << "\n";
  }

  void Write(const Observation &From, const ObservationEdge &Edge) {
    if (!OS)
      return;
    *OS << "  edge: ";
    *OS << From.getName() << " -> " << Edge.getNode().getName() << " (";
    *OS << (Edge.isDom() ? "dom" : "pdom") << ")\n";
  }

  template <typename T> void WriteTs(const T &V) { Write(V); }

  template <typename T1, typename T2, typename... Ts>
  void WriteTs(const T1 &V1, const T2 &V2, const Ts &...Vs) {
    Write(V1);
    WriteTs(V2, Vs...);
  }

  void CheckFailed(const Twine &Message) {
    if (OS)
      *OS << "MAIR verification failed: " << Message << "\n";
    Broken = true;
  }

  template <typename T1, typename... Ts>
  void CheckFailed(const Twine &Message, const T1 &V1, const Ts &...Vs) {
    CheckFailed(Message);
    if (OS)
      WriteTs(V1, Vs...);
  }

  template <typename... Ts>
  bool CheckRequired(bool Condition, const Twine &Message, const Ts &...Vs) {
    if (Condition)
      return true;
    CheckFailed(Message, Vs...);
    return false;
  }
};

} // namespace

#define Check(C, ...)                                                          \
  do {                                                                         \
    if (!(C)) {                                                                \
      CheckFailed(__VA_ARGS__);                                                \
    }                                                                          \
  } while (false)

class MAIRVerifier : public MAIRVerifierSupport {
  struct EdgeCounts {
    unsigned Dom = 0;
    unsigned PostDom = 0;

    void add(ObsEdgeKind Kind) {
      if (Kind == ObsEdgeKind::Dom)
        ++Dom;
      else
        ++PostDom;
    }
    unsigned get(ObsEdgeKind Kind) const {
      return Kind == ObsEdgeKind::Dom ? Dom : PostDom;
    }
  };

  using EdgeKey = std::pair<const Observation *, const Observation *>;

  const MAIRGraph *Graph = nullptr;
  DenseSet<const Observation *> LiveObservations;
  DenseMap<EdgeKey, EdgeCounts> ChildEdgeCounts;
  DenseMap<EdgeKey, EdgeCounts> ParentEdgeCounts;

public:
  explicit MAIRVerifier(raw_ostream *OS) : MAIRVerifierSupport(OS) {}

  bool verify(const MAIRGraph &G);

private:
  void verifyObservation(const Observation &Obs);
  void verifyMemoryAccess(const Observation &Obs, const MemoryAccess &Access);
  void verifyGuard(const Observation &Obs);
  void verifyScalarExpr(const MAIRScalarExpr &Expr, const MemoryAccess &Access,
                        const Observation &Obs);
  void verifyPeriodicRegion(const PeriodicRegion &Region,
                            const MemoryAccess &Access, const Observation &Obs);
  void verifyChildEdge(const Observation &From, const ObservationEdge &Edge);
  void verifyParentEdge(const Observation &To, const ObservationEdge &Edge);
  void verifyCounters();
  void verifyBoundaryRepresentatives();
  void verifyBoundaryRepresentative(const BasicBlock *BB,
                                    const Observation *Representative,
                                    bool IsEntry);
  void buildEdgeIndex();

  unsigned countChildEdge(const Observation &From, const Observation &To,
                          ObsEdgeKind Kind) const;
  unsigned countParentEdge(const Observation &To, const Observation &From,
                           ObsEdgeKind Kind) const;
};

bool MAIRVerifier::verify(const MAIRGraph &G) {
  Graph = &G;
  Broken = false;
  buildEdgeIndex();

  for (const Observation &Obs : G.Observations)
    verifyObservation(Obs);
  verifyBoundaryRepresentatives();
  verifyCounters();

  return !Broken;
}

void MAIRVerifier::buildEdgeIndex() {
  LiveObservations.clear();
  ChildEdgeCounts.clear();
  ParentEdgeCounts.clear();
  for (const Observation &Obs : Graph->Observations) {
    LiveObservations.insert(&Obs);
    for (const ObservationEdge &Edge : Obs.Children)
      ChildEdgeCounts[{&Obs, &Edge.getNode()}].add(Edge.getKind());
    for (const ObservationEdge &Edge : Obs.Parents)
      ParentEdgeCounts[{&Edge.getNode(), &Obs}].add(Edge.getKind());
  }
}

void MAIRVerifier::verifyBoundaryRepresentatives() {
  for (const auto &Entry : Graph->BBEntryRepresentatives)
    verifyBoundaryRepresentative(Entry.first, Entry.second, true);
  for (const auto &Entry : Graph->BBExitRepresentatives)
    verifyBoundaryRepresentative(Entry.first, Entry.second, false);
}

void MAIRVerifier::verifyBoundaryRepresentative(
    const BasicBlock *BB, const Observation *Representative, bool IsEntry) {
  const char *Kind = IsEntry ? "entry" : "exit";
  if (!CheckRequired(BB && Representative,
                     Twine("invalid BB-") + Kind + " representative"))
    return;
  if (!CheckRequired(LiveObservations.contains(Representative),
                     Twine("BB-") + Kind + " representative is not live",
                     Representative, BB))
    return;
  Check(Representative->getParent() == Graph,
        Twine("BB-") + Kind + " representative does not belong to graph",
        Representative, BB);
  Check(Representative->representsBlock(*BB),
        Twine("BB-") + Kind + " representative does not represent its block",
        Representative, BB);
  Check(!Representative->isEdgeSite(),
        Twine("BB-") + Kind + " representative is an edge site", Representative,
        BB);
  const bool HasExpectedSite = IsEntry ? Representative->isBasicBlockEntrySite()
                                       : Representative->isBasicBlockExitSite();
  Check(HasExpectedSite,
        Twine("BB-") + Kind + " representative is not a BB-" + Kind + " site",
        Representative, BB);
}

void MAIRVerifier::verifyObservation(const Observation &Obs) {
  if (!CheckRequired(Obs.getParent() == Graph,
                     "observation parent does not match graph", Obs))
    return;

  if (Obs.isEdgeSite()) {
    const BasicBlock *From = Obs.getEdgeFrom();
    const BasicBlock *To = Obs.getEdgeTo();
    if (!CheckRequired(From && To, "edge site has a null endpoint", Obs))
      return;
    Check(From != To, "edge site is a self edge", Obs, From, To);
    if (!CheckRequired(From->getParent() && To->getParent(),
                       "edge site endpoint has no function", Obs, From, To))
      return;
    Check(From->getParent() == To->getParent(), "edge site crosses functions",
          Obs, From, To);
    Check(From->getParent() == Graph->getFunction(),
          "edge site is outside graph function", Obs, From, To);
    Check(llvm::is_contained(successors(From), To),
          "edge site is not a current CFG edge", Obs, From, To);
    Check(&Obs.getAnchor() == From->getTerminator(),
          "edge observation is not anchored at its source terminator", Obs,
          From, &Obs.getAnchor());
  } else {
    const BasicBlock *BB = Obs.getBasicBlock();
    if (!CheckRequired(BB, "observation has no basic block", Obs))
      return;
    Check(BB->getParent() == Graph->getFunction(),
          "observation block is outside graph function", Obs, BB);

    const Instruction &Anchor = Obs.getAnchor();
    if (!CheckRequired(Anchor.getParent() == BB,
                       "observation anchor is outside its block", Obs, Anchor,
                       BB))
      return;

    if (Obs.isBasicBlockEntrySite()) {
      Check(&Anchor == &*BB->getFirstNonPHIIt(),
            "basic-block entry observation is not anchored at first non-PHI",
            Obs, &Anchor, BB);
    }

    if (Obs.isBasicBlockExitSite()) {
      Check(&Anchor == BB->getTerminator(),
            "basic-block exit observation is not anchored at terminator", Obs,
            &Anchor, BB);
    }
  }

  verifyGuard(Obs);

  for (const MemoryAccess &Access : Obs)
    verifyMemoryAccess(Obs, Access);

  for (const ObservationEdge &Edge : Obs.children())
    verifyChildEdge(Obs, Edge);
  for (const ObservationEdge &Edge : Obs.parents())
    verifyParentEdge(Obs, Edge);
}

void MAIRVerifier::verifyGuard(const Observation &Obs) {
  const auto *Guard = std::get_if<AnyProvenanceExecutedGuard>(&Obs.getGuard());
  if (!Guard)
    return;

  ArrayRef<Instruction *> Provenance = Guard->getProvenance();
  Check(!Provenance.empty(), "execution guard has empty provenance", Obs);
  SmallPtrSet<const Instruction *, 8> Seen;
  for (const Instruction *I : Provenance) {
    if (!CheckRequired(I, "execution guard has null provenance", Obs))
      return;
    Check(I->getFunction() == Graph->getFunction(),
          "execution guard provenance is outside graph function", Obs, I);
    Check(Seen.insert(I).second,
          "execution guard contains duplicate provenance", Obs, I);
  }
}

void MAIRVerifier::verifyMemoryAccess(const Observation &Obs,
                                      const MemoryAccess &Access) {
  Check(Access.getParent() == &Obs,
        "memory access parent does not match containing observation", Access,
        Obs);

  if (Obs.isMemoryInstSite()) {
    const Instruction *AccessI = Access.getInstruction();
    if (!CheckRequired(AccessI,
                       "memory observation has access without instruction",
                       Access, Obs))
      return;
    Check(AccessI->getFunction() == Obs.getAnchor().getFunction(),
          "memory access instruction is outside observation function", Access,
          Obs, AccessI, &Obs.getAnchor());
  }

  for (const Instruction *AccessI : Access.getInstructions()) {
    if (!CheckRequired(AccessI, "memory access has null provenance", Access,
                       Obs))
      return;
    Check(AccessI->getFunction() == Graph->getFunction(),
          "memory access provenance is outside graph function", Access, Obs,
          AccessI);
  }

  const Value *Base = Access.getBase();
  if (!CheckRequired(Base, "memory access has no base", Access, Obs))
    return;
  if (!CheckRequired(Access.getBaseUse().get(),
                     "memory access is not linked to a memory object", Access,
                     Obs, Base))
    return;
  const MemoryObject &Object = Access.getMemoryObject();
  Check(Object.getBase() == Base,
        "memory access is linked to an object for a different base", Access,
        Obs, Base);
  Check(Graph->getMemoryObject(*Base) == &Object,
        "memory access is not linked to the graph's canonical object", Access,
        Obs, Base);
  bool FoundUse = false;
  for (const MemoryAccess &User : Object.users())
    if (&User == &Access) {
      FoundUse = true;
      break;
    }
  Check(FoundUse, "memory object use-list does not contain this access", Access,
        Obs, Base);

  if (const PeriodicRegion *Region = Access.getPeriodicRegion())
    verifyPeriodicRegion(*Region, Access, Obs);
  if (const MaskedRegion *Region = Access.getMaskedRegion()) {
    Check(Region->getElementSize() != 0,
          "masked region has zero-sized elements", Access, Obs);
    if (const Value *Mask = Region->getDynamicMask()) {
      const auto *MaskType = dyn_cast<VectorType>(Mask->getType());
      if (!CheckRequired(MaskType && MaskType->getElementType()->isIntegerTy(1),
                         "masked region mask is not an i1 vector", Access, Obs,
                         Mask))
        return;
      Check(MaskType->getElementCount().getKnownMinValue() ==
                Region->getNumLanes(),
            "masked region lane count does not match mask", Access, Obs, Mask);
    } else {
      Check(Region->isConstantMask(),
            "masked region has neither dynamic nor constant mask", Access, Obs);
    }
  }
}

void MAIRVerifier::verifyScalarExpr(const MAIRScalarExpr &Expr,
                                    const MemoryAccess &Access,
                                    const Observation &Obs) {
  if (!CheckRequired(Expr.isValid(), "invalid scalar expression", Access, Obs))
    return;
  Check(Graph->ownsScalarExpr(Expr),
        "scalar expression belongs to another graph", Access, Obs);
  Type *Ty = Expr.getType();
  if (!CheckRequired(Ty, "scalar expression has no type", Access, Obs))
    return;

  ArrayRef<MAIRScalarExpr> Operands = Expr.operands();
  using Kind = MAIRScalarExpr::Kind;
  switch (Expr.getKind()) {
  case Kind::Constant:
    Check(Operands.empty(), "constant scalar expression has operands", Access,
          Obs);
    Check(Ty->isIntegerTy(), "scalar constant is not integer typed", Access,
          Obs);
    if (Ty->isIntegerTy())
      Check(cast<IntegerType>(Ty)->getBitWidth() ==
                Expr.getConstant().getBitWidth(),
            "scalar constant bit width does not match its type", Access, Obs);
    break;
  case Kind::LLVMValue: {
    Check(Operands.empty(), "LLVM-value scalar expression has operands", Access,
          Obs);
    const Value *V = Expr.getLLVMValue();
    if (!CheckRequired(V, "LLVM-value scalar expression is null", Access, Obs))
      return;
    Check(V->getType() == Ty,
          "LLVM-value scalar expression type does not match value", Access, Obs,
          V);
    if (const auto *I = dyn_cast<Instruction>(V))
      Check(I->getFunction() == Graph->getFunction(),
            "scalar LLVM value is outside graph function", Access, Obs, V);
    if (const auto *A = dyn_cast<Argument>(V))
      Check(A->getParent() == Graph->getFunction(),
            "scalar LLVM argument is outside graph function", Access, Obs, V);
    if (const auto *GV = dyn_cast<GlobalValue>(V))
      Check(GV->getParent() == Graph->getFunction()->getParent(),
            "scalar global is outside graph module", Access, Obs, V);
    break;
  }
  case Kind::Truncate:
  case Kind::ZeroExtend:
  case Kind::SignExtend:
    Check(Operands.size() == 1,
          "scalar cast expression does not have one operand", Access, Obs);
    break;
  case Kind::Add:
  case Kind::Mul:
  case Kind::UDiv:
  case Kind::SMin:
  case Kind::UMin:
  case Kind::SMax:
  case Kind::UMax:
    Check(Operands.size() == 2,
          "scalar binary expression does not have two operands", Access, Obs);
    break;
  }

  for (const MAIRScalarExpr &Operand : Operands)
    verifyScalarExpr(Operand, Access, Obs);

  if (llvm::any_of(Operands, [](const MAIRScalarExpr &Operand) {
        return !Operand.isValid() || !Operand.getType();
      }))
    return;
  if (Operands.empty())
    return;
  if (Expr.getKind() == Kind::Truncate || Expr.getKind() == Kind::ZeroExtend ||
      Expr.getKind() == Kind::SignExtend) {
    Check(Ty->isIntegerTy() && Operands.front().getType()->isIntegerTy(),
          "scalar cast operands are not integer typed", Access, Obs);
    if (Ty->isIntegerTy() && Operands.front().getType()->isIntegerTy()) {
      unsigned ResultWidth = cast<IntegerType>(Ty)->getBitWidth();
      unsigned OperandWidth =
          cast<IntegerType>(Operands.front().getType())->getBitWidth();
      if (Expr.getKind() == Kind::Truncate)
        Check(ResultWidth < OperandWidth, "scalar truncate does not narrow",
              Access, Obs);
      else
        Check(ResultWidth > OperandWidth, "scalar extension does not widen",
              Access, Obs);
    }
    return;
  }
  if (Expr.getKind() == Kind::Add && Ty->isPointerTy()) {
    Check(Operands.size() == 2 && Operands[0].getType() == Ty &&
              Operands[1].getType()->isIntegerTy(),
          "pointer add must use pointer plus integer byte offset", Access, Obs);
    return;
  }
  Check(Ty->isIntegerTy(), "scalar arithmetic result is not integer typed",
        Access, Obs);
  for (const MAIRScalarExpr &Operand : Operands)
    Check(Operand.getType() == Ty,
          "scalar arithmetic operand type does not match result", Access, Obs);
}

void MAIRVerifier::verifyPeriodicRegion(const PeriodicRegion &Region,
                                        const MemoryAccess &Access,
                                        const Observation &Obs) {
  if (!CheckRequired(Region.getBase(), "periodic region has no base", Access,
                     Obs))
    return;
  verifyScalarExpr(Region.getStart(), Access, Obs);
  verifyScalarExpr(Region.getStride(), Access, Obs);
  verifyScalarExpr(Region.getAccessSize(), Access, Obs);
  verifyScalarExpr(Region.getTripCount(), Access, Obs);

  if (!Region.getStart().isValid() || !Region.getStride().isValid() ||
      !Region.getAccessSize().isValid() || !Region.getTripCount().isValid())
    return;
  Check(Region.getStart().getType()->isPointerTy(),
        "periodic region start is not pointer typed", Access, Obs);
  Check(Region.getStride().getType()->isIntegerTy(),
        "periodic region stride is not integer typed", Access, Obs);
  Check(Region.getAccessSize().getType()->isIntegerTy(),
        "periodic region access size is not integer typed", Access, Obs);
  Check(Region.getTripCount().getType()->isIntegerTy(),
        "periodic region trip count is not integer typed", Access, Obs);
  if (Region.getStride().getKind() == MAIRScalarExpr::Kind::Constant)
    Check(!Region.getStride().getConstant().isZero(),
          "periodic region has zero stride", Access, Obs);
  if (Region.getAccessSize().getKind() == MAIRScalarExpr::Kind::Constant)
    Check(!Region.getAccessSize().getConstant().isZero(),
          "periodic region has zero access size", Access, Obs);
}

void MAIRVerifier::verifyChildEdge(const Observation &From,
                                   const ObservationEdge &Edge) {
  const Observation &To = Edge.getNode();
  if (!CheckRequired(
          To.getParent() == Graph,
          "child edge destination observation does not belong to graph", From))
    return;
  Check(countChildEdge(From, To, Edge.getKind()) == 1,
        "child edge appears more than once", From);
  Check(countParentEdge(To, From, Edge.getKind()) == 1,
        "child edge is missing from destination parent list", From);
}

void MAIRVerifier::verifyParentEdge(const Observation &To,
                                    const ObservationEdge &Edge) {
  const Observation &From = Edge.getNode();
  if (!CheckRequired(From.getParent() == Graph,
                     "parent edge source observation does not belong to graph",
                     To))
    return;
  Check(countParentEdge(To, From, Edge.getKind()) == 1,
        "parent edge appears more than once", To);
  Check(countChildEdge(From, To, Edge.getKind()) == 1,
        "parent edge is missing from source child list", To);
}

void MAIRVerifier::verifyCounters() {
  unsigned ObservedNodes = 0;
  unsigned ObservedChildEdges = 0;
  unsigned ObservedParentEdges = 0;
  for (const Observation &Obs : Graph->Observations) {
    ++ObservedNodes;
    ObservedChildEdges += Obs.Children.size();
    ObservedParentEdges += Obs.Parents.size();

    unsigned ObservedAccesses = 0;
    for (const MemoryAccess &Access : Obs) {
      (void)Access;
      ++ObservedAccesses;
    }
    Check(ObservedAccesses == Obs.size(),
          "observation access count does not match access list", Obs);
  }

  Check(ObservedNodes == Graph->NumObservations,
        "graph observation count does not match observation list");
  Check(ObservedChildEdges == Graph->NumEdges,
        "graph edge count does not match child edge lists");
  Check(ObservedParentEdges == Graph->NumEdges,
        "graph edge count does not match parent edge lists");
}

unsigned MAIRVerifier::countChildEdge(const Observation &From,
                                      const Observation &To,
                                      ObsEdgeKind Kind) const {
  auto It = ChildEdgeCounts.find({&From, &To});
  return It == ChildEdgeCounts.end() ? 0 : It->second.get(Kind);
}

unsigned MAIRVerifier::countParentEdge(const Observation &To,
                                       const Observation &From,
                                       ObsEdgeKind Kind) const {
  auto It = ParentEdgeCounts.find({&From, &To});
  return It == ParentEdgeCounts.end() ? 0 : It->second.get(Kind);
}

bool MAIRGraph::verify(raw_ostream *OS) const {
  if (!isLive()) {
    if (OS)
      *OS << "MAIRGraph is not live\n";
    return false;
  }
  // Keep the no-diagnostics path cheap; printing MAIR objects is relatively
  // expensive and should only happen on explicit request.
  MAIRVerifier Verifier(OS);
  return Verifier.verify(*this);
}

} // namespace mair

#undef Check
