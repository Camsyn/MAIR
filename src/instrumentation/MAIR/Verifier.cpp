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

  void Write(const ProgramPoint *PP) {
    if (!PP)
      return;
    Write(*PP);
  }

  void Write(const ProgramPoint &PP) {
    if (!OS)
      return;
    *OS << "  program point: ";
    PP.print(*OS);
    *OS << "\n";
  }

  void Write(const Observation &Obs) {
    if (!OS)
      return;
    *OS << "  observation: ";
    Obs.print(*OS);
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

  void Write(const ProgramPoint &From, const ProgramPointEdge &Edge) {
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

    void add(PPEdgeKind Kind) {
      if (Kind == PPEdgeKind::Dom)
        ++Dom;
      else
        ++PostDom;
    }
    unsigned get(PPEdgeKind Kind) const {
      return Kind == PPEdgeKind::Dom ? Dom : PostDom;
    }
  };

  using EdgeKey = std::pair<const ProgramPoint *, const ProgramPoint *>;

  const MAIRGraph *Graph = nullptr;
  DenseSet<const ProgramPoint *> LiveProgramPoints;
  DenseMap<EdgeKey, EdgeCounts> ChildEdgeCounts;
  DenseMap<EdgeKey, EdgeCounts> ParentEdgeCounts;
  std::optional<DominatorTree> GuardDominance;

public:
  explicit MAIRVerifier(raw_ostream *OS) : MAIRVerifierSupport(OS) {}

  bool verify(const MAIRGraph &G);

private:
  void verifyProgramPoint(const ProgramPoint &PP);
  void verifyLoopHierarchy();
  void verifyLoopProgramPoint(const LoopProgramPoint &PP);
  void verifyObservation(const ProgramPoint &PP, const Observation &Obs);
  void verifyGuard(const ProgramPoint &PP);
  void verifyScalarExpr(const MAIRScalarExpr &Expr, const Observation &Obs,
                        const ProgramPoint &PP);
  void verifyPeriodicRegion(const PeriodicRegion &Region,
                            const Observation &Obs, const ProgramPoint &PP);
  void verifyChildEdge(const ProgramPoint &From, const ProgramPointEdge &Edge);
  void verifyParentEdge(const ProgramPoint &To, const ProgramPointEdge &Edge);
  void verifyCounters();
  void verifyBoundaryRepresentatives();
  void verifyBoundaryRepresentative(const BasicBlock *BB,
                                    const ProgramPoint *Representative,
                                    bool IsEntry);
  void buildEdgeIndex();

  unsigned countChildEdge(const ProgramPoint &From, const ProgramPoint &To,
                          PPEdgeKind Kind) const;
  unsigned countParentEdge(const ProgramPoint &To, const ProgramPoint &From,
                           PPEdgeKind Kind) const;
};

bool MAIRVerifier::verify(const MAIRGraph &G) {
  Graph = &G;
  Broken = false;
  if (!G.ProgramPoints.empty() &&
      !CheckRequired(G.getFunction(), "non-empty graph has no function"))
    return false;
  buildEdgeIndex();

  for (const ProgramPoint &PP : G.ProgramPoints)
    verifyProgramPoint(PP);
  verifyLoopHierarchy();
  verifyBoundaryRepresentatives();
  verifyCounters();

  return !Broken;
}

void MAIRVerifier::verifyLoopHierarchy() {
  if (Graph->Parent) {
    Check(Graph->ScopeLoop, "child graph has no loop");
    Check(Graph->Resources == Graph->Parent->Resources,
          "child graph does not share its parent's resources");
    Check(!Graph->OwnedResources, "child graph owns separate resources");
    Check(Graph->F == Graph->Parent->F,
          "child graph belongs to another function");
    if (Graph->ScopeLoop)
      Check(Graph->ScopeLoop->getParentLoop() == Graph->Parent->ScopeLoop,
            "child graph loop does not match parent scope");
    if (CheckRequired(Graph->LoopPreheader,
                      "loop graph has no preheader boundary")) {
      Check(Graph->LoopPreheader->getParent() == Graph->Parent,
            "loop preheader boundary does not belong to parent graph");
      Check(Graph->LoopPreheader->isLoopPreheader() &&
                &Graph->LoopPreheader->getLoopGraph() == Graph,
            "loop graph preheader boundary refers to another loop");
    }
    SmallPtrSet<const BasicBlock *, 4> ExitBlocks;
    for (const LoopProgramPoint *Exit : Graph->LoopExits) {
      if (!CheckRequired(Exit, "loop graph has a null exit boundary"))
        continue;
      Check(Exit->getParent() == Graph->Parent,
            "loop exit boundary does not belong to parent graph");
      Check(Exit->isLoopExit() && &Exit->getLoopGraph() == Graph,
            "loop graph exit boundary refers to another loop");
      Check(ExitBlocks.insert(Exit->getBasicBlock()).second,
            "loop graph has duplicate boundaries for an exit block");
    }
    if (Graph->ScopeLoop) {
      SmallVector<BasicBlock *, 4> ExpectedExits;
      Graph->ScopeLoop->getUniqueExitBlocks(ExpectedExits);
      Check(ExpectedExits.size() == ExitBlocks.size(),
            "loop graph exit boundaries do not match loop exits");
      for (const BasicBlock *Exit : ExpectedExits)
        Check(ExitBlocks.contains(Exit),
              "loop graph is missing a loop exit boundary");
    }
  } else {
    Check(!Graph->ScopeLoop, "root graph has a loop scope");
    Check(Graph->OwnedResources &&
              Graph->Resources == Graph->OwnedResources.get(),
          "root graph does not own its resources");
  }
  SmallPtrSet<const llvm::Loop *, 4> SeenLoops;
  for (const auto &Child : Graph->ChildGraphs) {
    if (!CheckRequired(Child.get(), "graph has a null child graph"))
      continue;
    Check(Child->Parent == Graph, "child graph parent does not match owner");
    Check(SeenLoops.insert(Child->ScopeLoop).second,
          "graph has duplicate child graphs for one loop");
    unsigned Preheaders = 0;
    unsigned Exits = 0;
    for (const ProgramPoint &PP : Graph->ProgramPoints) {
      const auto *Boundary = dyn_cast<LoopProgramPoint>(&PP);
      if (!Boundary || &Boundary->getLoopGraph() != Child.get())
        continue;
      Preheaders += Boundary->isLoopPreheader();
      Exits += Boundary->isLoopExit();
    }
    Check(Preheaders == 1, "child graph must have one preheader boundary");
    Check(Exits == Child->LoopExits.size(),
          "child graph exit boundary list does not match parent points");
  }
}

void MAIRVerifier::verifyLoopProgramPoint(const LoopProgramPoint &PP) {
  const MAIRGraph &Body = PP.getLoopGraph();
  Check(Body.getParent() == Graph,
        "loop boundary body is not a child of its containing graph", PP);
  Check(llvm::any_of(Graph->ChildGraphs,
                     [&](const auto &Child) { return Child.get() == &Body; }),
        "loop boundary body is not owned by its containing graph", PP);
  const llvm::Loop *Loop = Body.getLoop();
  if (!CheckRequired(Loop, "loop boundary body has no loop", PP))
    return;
  if (PP.isLoopPreheader()) {
    Check(PP.isBasicBlockExitSite(), "loop preheader is not a BB-exit site",
          PP);
    Check(PP.getBasicBlock() == Loop->getLoopPreheader(),
          "loop preheader is not at its loop's preheader", PP);
    Check(Body.getLoopPreheader() == &PP,
          "loop preheader is not registered in its body", PP);
  } else {
    Check(PP.isBasicBlockEntrySite(), "loop exit is not a BB-entry site", PP);
    Check(!Loop->contains(PP.getBasicBlock()) &&
              llvm::any_of(
                  predecessors(PP.getBasicBlock()),
                  [&](const BasicBlock *Pred) { return Loop->contains(Pred); }),
          "loop exit is not at an exit block of its loop", PP);
    Check(llvm::is_contained(Body.getLoopExits(), &PP),
          "loop exit is not registered in its body", PP);
  }
}

void MAIRVerifier::buildEdgeIndex() {
  LiveProgramPoints.clear();
  ChildEdgeCounts.clear();
  ParentEdgeCounts.clear();
  for (const ProgramPoint &PP : Graph->ProgramPoints) {
    LiveProgramPoints.insert(&PP);
    for (const ProgramPointEdge &Edge : PP.Children)
      ChildEdgeCounts[{&PP, &Edge.getNode()}].add(Edge.getKind());
    for (const ProgramPointEdge &Edge : PP.Parents)
      ParentEdgeCounts[{&Edge.getNode(), &PP}].add(Edge.getKind());
  }
}

void MAIRVerifier::verifyBoundaryRepresentatives() {
  for (const auto &Entry : Graph->BBEntryRepresentatives)
    verifyBoundaryRepresentative(Entry.first, Entry.second, true);
  for (const auto &Entry : Graph->BBExitRepresentatives)
    verifyBoundaryRepresentative(Entry.first, Entry.second, false);
}

void MAIRVerifier::verifyBoundaryRepresentative(
    const BasicBlock *BB, const ProgramPoint *Representative, bool IsEntry) {
  const char *Kind = IsEntry ? "entry" : "exit";
  if (!CheckRequired(BB && Representative,
                     Twine("invalid BB-") + Kind + " representative"))
    return;
  if (!CheckRequired(LiveProgramPoints.contains(Representative),
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

void MAIRVerifier::verifyProgramPoint(const ProgramPoint &PP) {
  if (!CheckRequired(PP.getParent() == Graph,
                     "program point parent does not match graph", PP))
    return;
  if (const auto *LoopPP = dyn_cast<LoopProgramPoint>(&PP))
    verifyLoopProgramPoint(*LoopPP);
  else
    Check(Graph->containsBlock(*PP.getBasicBlock()),
          "ordinary program point is outside its graph's loop scope", PP);

  if (PP.isEdgeSite()) {
    const BasicBlock *From = PP.getEdgeFrom();
    const BasicBlock *To = PP.getEdgeTo();
    if (!CheckRequired(From && To, "edge site has a null endpoint", PP))
      return;
    Check(From != To, "edge site is a self edge", PP, From, To);
    if (!CheckRequired(From->getParent() && To->getParent(),
                       "edge site endpoint has no function", PP, From, To))
      return;
    Check(From->getParent() == To->getParent(), "edge site crosses functions",
          PP, From, To);
    Check(From->getParent() == Graph->getFunction(),
          "edge site is outside graph function", PP, From, To);
    Check(Graph->containsBlock(*From) && Graph->containsBlock(*To),
          "edge site crosses graph loop scopes", PP, From, To);
    Check(llvm::is_contained(successors(From), To),
          "edge site is not a current CFG edge", PP, From, To);
    Check(&PP.getAnchor() == From->getTerminator(),
          "edge program point is not anchored at its source terminator", PP,
          From, &PP.getAnchor());
  } else {
    const BasicBlock *BB = PP.getBasicBlock();
    if (!CheckRequired(BB, "program point has no basic block", PP))
      return;
    Check(BB->getParent() == Graph->getFunction(),
          "program point block is outside graph function", PP, BB);

    const Instruction &Anchor = PP.getAnchor();
    if (!CheckRequired(Anchor.getParent() == BB,
                       "program point anchor is outside its block", PP, Anchor,
                       BB))
      return;

    if (PP.isBasicBlockEntrySite()) {
      Check(&Anchor == &*BB->getFirstNonPHIIt(),
            "basic-block entry program point is not anchored at first non-PHI",
            PP, &Anchor, BB);
    }

    if (PP.isBasicBlockExitSite()) {
      Check(&Anchor == BB->getTerminator(),
            "basic-block exit program point is not anchored at terminator", PP,
            &Anchor, BB);
    }
  }

  verifyGuard(PP);

  for (const Observation &Obs : PP)
    verifyObservation(PP, Obs);

  for (const ProgramPointEdge &Edge : PP.children())
    verifyChildEdge(PP, Edge);
  for (const ProgramPointEdge &Edge : PP.parents())
    verifyParentEdge(PP, Edge);
}

void MAIRVerifier::verifyGuard(const ProgramPoint &PP) {
  const auto *Guard = std::get_if<AnyProvenanceExecutedGuard>(&PP.getGuard());
  if (!Guard)
    return;

  ArrayRef<Instruction *> Provenance = Guard->getProvenance();
  Check(!Provenance.empty(), "execution guard has empty provenance", PP);
  SmallPtrSet<const Instruction *, 8> Seen;
  const BasicBlock *Reset = Guard->getResetBlock();
  const DominatorTree *DT = Graph->getRoot().Analyses.DT;
  if (Reset) {
    if (!CheckRequired(Reset->getParent() == Graph->getFunction(),
                       "execution guard reset block is outside graph function",
                       PP, Reset))
      return;
    if (!DT) {
      if (!GuardDominance)
        GuardDominance.emplace(*const_cast<Function *>(Graph->getFunction()));
      DT = &*GuardDominance;
    }
    Check(DT->dominates(Reset, PP.getBasicBlock()),
          "execution guard reset block does not dominate its site", PP, Reset);
  }
  for (const Instruction *I : Provenance) {
    if (!CheckRequired(I, "execution guard has null provenance", PP))
      return;
    if (!CheckRequired(I->getFunction() == Graph->getFunction(),
                       "execution guard provenance is outside graph function",
                       PP, I))
      return;
    Check(Seen.insert(I).second,
          "execution guard contains duplicate provenance", PP, I);
    if (Reset) {
      Check(I->getParent() != Reset,
            "execution guard provenance is in its reset block", PP, I);
      Check(DT->dominates(Reset, I->getParent()),
            "execution guard reset block does not dominate provenance", PP, I);
    }
  }
}

void MAIRVerifier::verifyObservation(const ProgramPoint &PP,
                                     const Observation &Obs) {
  Check(Obs.getParent() == &PP,
        "observation parent does not match containing program point", Obs, PP);

  if (PP.isMemoryInstSite()) {
    const Instruction *Origin = Obs.getInstruction();
    if (!CheckRequired(Origin,
                       "memory-instruction program point has observation "
                       "without instruction",
                       Obs, PP))
      return;
    Check(Origin->getFunction() == PP.getAnchor().getFunction(),
          "observation instruction is outside program point function", Obs, PP,
          Origin, &PP.getAnchor());
  }

  for (const Instruction *Origin : Obs.getInstructions()) {
    if (!CheckRequired(Origin, "observation has null provenance", Obs, PP))
      return;
    Check(Origin->getFunction() == Graph->getFunction(),
          "observation provenance is outside graph function", Obs, PP, Origin);
  }

  const Value *Base = Obs.getBase();
  if (!CheckRequired(Base, "observation has no base", Obs, PP))
    return;
  if (!CheckRequired(Obs.getBaseUse().get(),
                     "observation is not linked to a memory object", Obs, PP,
                     Base))
    return;
  const MemoryObject &Object = Obs.getMemoryObject();
  Check(Object.getBase() == Base,
        "observation is linked to an object for a different base", Obs, PP,
        Base);
  Check(Graph->getMemoryObject(*Base) == &Object,
        "observation is not linked to the graph's canonical object", Obs, PP,
        Base);
  bool FoundUse = false;
  for (const Observation &User : Object.users())
    if (&User == &Obs) {
      FoundUse = true;
      break;
    }
  Check(FoundUse, "memory object use-list does not contain this observation",
        Obs, PP, Base);

  if (const PeriodicRegion *Region = Obs.getPeriodicRegion())
    verifyPeriodicRegion(*Region, Obs, PP);
  if (const MaskedRegion *Region = Obs.getMaskedRegion()) {
    Check(Region->getElementSize() != 0,
          "masked region has zero-sized elements", Obs, PP);
    if (const Value *Mask = Region->getDynamicMask()) {
      const auto *MaskType = dyn_cast<VectorType>(Mask->getType());
      if (!CheckRequired(MaskType && MaskType->getElementType()->isIntegerTy(1),
                         "masked region mask is not an i1 vector", Obs, PP,
                         Mask))
        return;
      Check(MaskType->getElementCount().getKnownMinValue() ==
                Region->getNumLanes(),
            "masked region lane count does not match mask", Obs, PP, Mask);
    } else {
      Check(Region->isConstantMask(),
            "masked region has neither dynamic nor constant mask", Obs, PP);
    }
  }
}

void MAIRVerifier::verifyScalarExpr(const MAIRScalarExpr &Expr,
                                    const Observation &Obs,
                                    const ProgramPoint &PP) {
  if (!CheckRequired(Expr.isValid(), "invalid scalar expression", Obs, PP))
    return;
  Check(Graph->ownsScalarExpr(Expr),
        "scalar expression belongs to another graph", Obs, PP);
  Type *Ty = Expr.getType();
  if (!CheckRequired(Ty, "scalar expression has no type", Obs, PP))
    return;

  ArrayRef<MAIRScalarExpr> Operands = Expr.operands();
  using Kind = MAIRScalarExpr::Kind;
  switch (Expr.getKind()) {
  case Kind::Constant:
    Check(Operands.empty(), "constant scalar expression has operands", Obs, PP);
    Check(Ty->isIntegerTy(), "scalar constant is not integer typed", Obs, PP);
    if (Ty->isIntegerTy())
      Check(cast<IntegerType>(Ty)->getBitWidth() ==
                Expr.getConstant().getBitWidth(),
            "scalar constant bit width does not match its type", Obs, PP);
    break;
  case Kind::LLVMValue: {
    Check(Operands.empty(), "LLVM-value scalar expression has operands", Obs,
          PP);
    const Value *V = Expr.getLLVMValue();
    if (!CheckRequired(V, "LLVM-value scalar expression is null", Obs, PP))
      return;
    Check(V->getType() == Ty,
          "LLVM-value scalar expression type does not match value", Obs, PP, V);
    if (const auto *I = dyn_cast<Instruction>(V))
      Check(I->getFunction() == Graph->getFunction(),
            "scalar LLVM value is outside graph function", Obs, PP, V);
    if (const auto *A = dyn_cast<Argument>(V))
      Check(A->getParent() == Graph->getFunction(),
            "scalar LLVM argument is outside graph function", Obs, PP, V);
    if (const auto *GV = dyn_cast<GlobalValue>(V))
      Check(GV->getParent() == Graph->getFunction()->getParent(),
            "scalar global is outside graph module", Obs, PP, V);
    break;
  }
  case Kind::Truncate:
  case Kind::ZeroExtend:
  case Kind::SignExtend:
    Check(Operands.size() == 1,
          "scalar cast expression does not have one operand", Obs, PP);
    break;
  case Kind::Add:
  case Kind::Mul:
  case Kind::UDiv:
  case Kind::SMin:
  case Kind::UMin:
  case Kind::SMax:
  case Kind::UMax:
    Check(Operands.size() == 2,
          "scalar binary expression does not have two operands", Obs, PP);
    break;
  }

  for (const MAIRScalarExpr &Operand : Operands)
    verifyScalarExpr(Operand, Obs, PP);

  if (llvm::any_of(Operands, [](const MAIRScalarExpr &Operand) {
        return !Operand.isValid() || !Operand.getType();
      }))
    return;
  if (Operands.empty())
    return;
  if (Expr.getKind() == Kind::Truncate || Expr.getKind() == Kind::ZeroExtend ||
      Expr.getKind() == Kind::SignExtend) {
    Check(Ty->isIntegerTy() && Operands.front().getType()->isIntegerTy(),
          "scalar cast operands are not integer typed", Obs, PP);
    if (Ty->isIntegerTy() && Operands.front().getType()->isIntegerTy()) {
      unsigned ResultWidth = cast<IntegerType>(Ty)->getBitWidth();
      unsigned OperandWidth =
          cast<IntegerType>(Operands.front().getType())->getBitWidth();
      if (Expr.getKind() == Kind::Truncate)
        Check(ResultWidth < OperandWidth, "scalar truncate does not narrow",
              Obs, PP);
      else
        Check(ResultWidth > OperandWidth, "scalar extension does not widen",
              Obs, PP);
    }
    return;
  }
  if (Expr.getKind() == Kind::Add && Ty->isPointerTy()) {
    Check(Operands.size() == 2 && Operands[0].getType() == Ty &&
              Operands[1].getType()->isIntegerTy(),
          "pointer add must use pointer plus integer byte offset", Obs, PP);
    return;
  }
  Check(Ty->isIntegerTy(), "scalar arithmetic result is not integer typed", Obs,
        PP);
  for (const MAIRScalarExpr &Operand : Operands)
    Check(Operand.getType() == Ty,
          "scalar arithmetic operand type does not match result", Obs, PP);
}

void MAIRVerifier::verifyPeriodicRegion(const PeriodicRegion &Region,
                                        const Observation &Obs,
                                        const ProgramPoint &PP) {
  if (!CheckRequired(Region.getBase(), "periodic region has no base", Obs, PP))
    return;
  verifyScalarExpr(Region.getStart(), Obs, PP);
  verifyScalarExpr(Region.getStride(), Obs, PP);
  verifyScalarExpr(Region.getAccessSize(), Obs, PP);
  verifyScalarExpr(Region.getTripCount(), Obs, PP);

  if (!Region.getStart().isValid() || !Region.getStride().isValid() ||
      !Region.getAccessSize().isValid() || !Region.getTripCount().isValid())
    return;
  Check(Region.getStart().getType()->isPointerTy(),
        "periodic region start is not pointer typed", Obs, PP);
  Check(Region.getStride().getType()->isIntegerTy(),
        "periodic region stride is not integer typed", Obs, PP);
  Check(Region.getAccessSize().getType()->isIntegerTy(),
        "periodic region access size is not integer typed", Obs, PP);
  Check(Region.getTripCount().getType()->isIntegerTy(),
        "periodic region trip count is not integer typed", Obs, PP);
  if (Region.getStride().getKind() == MAIRScalarExpr::Kind::Constant)
    Check(!Region.getStride().getConstant().isZero(),
          "periodic region has zero stride", Obs, PP);
  if (Region.getAccessSize().getKind() == MAIRScalarExpr::Kind::Constant)
    Check(!Region.getAccessSize().getConstant().isZero(),
          "periodic region has zero access size", Obs, PP);
}

void MAIRVerifier::verifyChildEdge(const ProgramPoint &From,
                                   const ProgramPointEdge &Edge) {
  const ProgramPoint &To = Edge.getNode();
  if (!CheckRequired(
          To.getParent() == Graph,
          "child edge destination program point does not belong to graph",
          From))
    return;
  Check(countChildEdge(From, To, Edge.getKind()) == 1,
        "child edge appears more than once", From);
  Check(countParentEdge(To, From, Edge.getKind()) == 1,
        "child edge is missing from destination parent list", From);
}

void MAIRVerifier::verifyParentEdge(const ProgramPoint &To,
                                    const ProgramPointEdge &Edge) {
  const ProgramPoint &From = Edge.getNode();
  if (!CheckRequired(
          From.getParent() == Graph,
          "parent edge source program point does not belong to graph", To))
    return;
  Check(countParentEdge(To, From, Edge.getKind()) == 1,
        "parent edge appears more than once", To);
  Check(countChildEdge(From, To, Edge.getKind()) == 1,
        "parent edge is missing from source child list", To);
}

void MAIRVerifier::verifyCounters() {
  unsigned ObservedProgramPoints = 0;
  unsigned ObservedChildEdges = 0;
  unsigned ObservedParentEdges = 0;
  for (const ProgramPoint &PP : Graph->ProgramPoints) {
    ++ObservedProgramPoints;
    ObservedChildEdges += PP.Children.size();
    ObservedParentEdges += PP.Parents.size();

    unsigned ObservedObservations = 0;
    for (const Observation &Obs : PP) {
      (void)Obs;
      ++ObservedObservations;
    }
    Check(ObservedObservations == PP.size(),
          "program point observation count does not match observation list",
          PP);
  }

  Check(ObservedProgramPoints == Graph->NumProgramPoints,
        "graph program point count does not match program point list");
  Check(ObservedChildEdges == Graph->NumEdges,
        "graph edge count does not match child edge lists");
  Check(ObservedParentEdges == Graph->NumEdges,
        "graph edge count does not match parent edge lists");
}

unsigned MAIRVerifier::countChildEdge(const ProgramPoint &From,
                                      const ProgramPoint &To,
                                      PPEdgeKind Kind) const {
  auto It = ChildEdgeCounts.find({&From, &To});
  return It == ChildEdgeCounts.end() ? 0 : It->second.get(Kind);
}

unsigned MAIRVerifier::countParentEdge(const ProgramPoint &To,
                                       const ProgramPoint &From,
                                       PPEdgeKind Kind) const {
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
  bool Valid = true;
  for (const MAIRGraph *Graph : graphs()) {
    if (!Graph->isLive()) {
      if (OS)
        *OS << "MAIR loop graph is not live\n";
      Valid = false;
      continue;
    }
    MAIRVerifier Verifier(OS);
    Valid &= Verifier.verify(*Graph);
  }
  return Valid;
}

} // namespace mair

#undef Check
