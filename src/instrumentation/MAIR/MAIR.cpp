#include "MAIR/MAIR.h"

#include "MAIR/MAIRInternal.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

struct MAIRScopeTree {
  mair::MAIRGraph *Root = nullptr;
};

mair::MAIRGraph *scopeChild(const std::unique_ptr<mair::MAIRGraph> &Child) {
  return Child.get();
}

} // namespace

namespace llvm {

template <> struct GraphTraits<MAIRScopeTree *> {
  using NodeRef = mair::MAIRGraph *;
  using ChildIteratorType = decltype(map_iterator(
      std::declval<NodeRef>()->children().begin(), scopeChild));

  static NodeRef getEntryNode(MAIRScopeTree *Tree) { return Tree->Root; }

  static ChildIteratorType child_begin(NodeRef Scope) {
    return map_iterator(Scope->children().begin(), scopeChild);
  }

  static ChildIteratorType child_end(NodeRef Scope) {
    return map_iterator(Scope->children().end(), scopeChild);
  }
};

} // namespace llvm

void mair::MAIRGraph::propagateBarrier() {
  MAIRScopeTree Tree{this};
  for (MAIRGraph *Scope : post_order(&Tree))
    for (const auto &Child : Scope->ChildGraphs) {
      Scope->ForwardBarrier |= Child->ForwardBarrier;
      Scope->BackwardBarrier |= Child->BackwardBarrier;
    }
}

namespace mair {

namespace {

// The edge of kind \p Kind to \p Node in \p Edges, or the end of the list.
template <class EdgeListT>
static auto findEdge(EdgeListT &Edges, const ProgramPoint &Node,
                     PPEdgeKind Kind) {
  return llvm::find_if(Edges, [&](const ProgramPointEdge &Edge) {
    return &Edge.getNode() == &Node && Edge.getKind() == Kind;
  });
}

static void printEdgeList(raw_ostream &OS, ArrayRef<ProgramPointEdge> Edges) {
  ListSeparator Separator;
  for (const ProgramPointEdge &Edge : Edges) {
    OS << Separator << Edge.getNode().getName() << "("
       << getEdgeKindName(Edge.getKind());
    if (const Cycle *Iterated = Edge.getIteratedCycle()) {
      OS << " iter ";
      printValueName(OS, *Iterated->getHeader());
    }
    OS << ")";
  }
}

} // namespace

StringRef getEdgeKindName(PPEdgeKind Kind) {
  switch (Kind) {
  case PPEdgeKind::Dom:
    return "dom";
  case PPEdgeKind::PostDom:
    return "pdom";
  }
  llvm_unreachable("unknown MAIR program point edge kind");
}

ProgramPointSite ProgramPointSite::anchored(const Instruction &Anchor, Kind K) {
  assert(K != Kind::Edge && "an anchored site cannot have edge kind");
  assert(Anchor.getParent() && "a program point anchor must belong to a block");
  ProgramPointSite Site;
  Site.SiteKind = K;
  Site.Anchor = &Anchor;
  return Site;
}

ProgramPointSite ProgramPointSite::edge(const BasicBlock &From,
                                        const BasicBlock &To) {
  const Instruction *Anchor = From.getTerminator();
  assert(Anchor && "an edge program point source must have a terminator");
  ProgramPointSite Site;
  Site.SiteKind = Kind::Edge;
  Site.Anchor = Anchor;
  Site.From = &From;
  Site.To = &To;
  return Site;
}

StringRef ProgramPointSite::getKindName(Kind K) {
  switch (K) {
  case Kind::MemoryInst:
    return "memory";
  case Kind::BasicBlockEntry:
    return "bb.entry";
  case Kind::BasicBlockExit:
    return "bb.exit";
  case Kind::Edge:
    return "edge";
  }
  llvm_unreachable("unknown MAIR program point site kind");
}

void ProgramPointSite::print(raw_ostream &OS) const {
  auto PrintBlock = [&](const BasicBlock *BB) {
    if (BB)
      printValueName(OS, *BB);
    else
      OS << "null";
  };
  OS << getKindName(SiteKind) << "(";
  if (isEdge()) {
    PrintBlock(From);
    OS << " -> ";
    PrintBlock(To);
  } else {
    PrintBlock(getBasicBlock());
    if (SiteKind == Kind::MemoryInst) {
      OS << ", ";
      printValueName(OS, getAnchor());
    }
  }
  OS << ")";
}

void AnyProvenanceExecutedGuard::print(raw_ostream &OS) const {
  OS << "any(";
  ListSeparator Separator;
  for (const Instruction *I : Provenance) {
    OS << Separator;
    if (I)
      printValueName(OS, *I);
    else
      OS << "null";
  }
  OS << ")";
}

bool AnyProvenanceExecutedGuard::operator==(
    const AnyProvenanceExecutedGuard &Other) const {
  if (ResetBlock != Other.ResetBlock ||
      Provenance.size() != Other.Provenance.size())
    return false;
  return llvm::all_of(Provenance,
                      [&](const Instruction *I) {
                        return llvm::is_contained(Other.Provenance, I);
                      }) &&
         llvm::all_of(Other.Provenance, [&](const Instruction *I) {
           return llvm::is_contained(Provenance, I);
         });
}

ProgramPoint::ProgramPoint(StringRef N, const Instruction &Anchor, SiteKind K)
    : ProgramPoint(N, ProgramPointSite::anchored(Anchor, K), AlwaysGuard{}) {}

ProgramPoint::ProgramPoint(StringRef N, ProgramPointSite S, ProgramPointGuard G)
    : Name(N.str()), Site(std::move(S)), Guard(std::move(G)) {
  assert(Site.getBasicBlock() && "a program point must have an anchor");
  if (!Site.isEdge())
    RepresentedBlocks.insert(Site.getBasicBlock());
}

ProgramPoint::~ProgramPoint() {
  Observations.clearAndDispose(std::default_delete<Observation>());
}

LoopProgramPoint::LoopProgramPoint(StringRef Name, ProgramPointSite Site,
                                   BoundaryKind Kind, MAIRGraph &LoopGraph)
    : ProgramPoint(Name, std::move(Site), AlwaysGuard{}), Boundary(Kind),
      Body(&LoopGraph) {
  PointKind = ProgramPoint::Kind::Loop;
}

Observation &ProgramPoint::addObservation(const Observation &A) {
  auto &Node = *new Observation(A.getInstructions(), A.getRegion(), A.getKind(),
                                A.getProperties());
  Observations.push_back(Node);
  Node.setParent(this);
  if (MAIRGraph *Graph = getParent())
    Graph->registerObservation(Node);
  ++NumObservations;
  Sorted = false;
  return Node;
}

const ProgramPointEdge *ProgramPoint::findChild(const ProgramPoint &PP,
                                                PPEdgeKind Kind) const {
  auto It = findEdge(Children, PP, Kind);
  return It == Children.end() ? nullptr : &*It;
}

const ProgramPointEdge *ProgramPoint::findParent(const ProgramPoint &PP,
                                                 PPEdgeKind Kind) const {
  auto It = findEdge(Parents, PP, Kind);
  return It == Parents.end() ? nullptr : &*It;
}

bool ProgramPoint::hasCompatibleMergeSemantics(
    const ProgramPoint &Other) const {
  if (isLoopProgramPoint() || Other.isLoopProgramPoint())
    return false;
  if (Guard != Other.Guard)
    return false;
  if (!isEdgeSite() && !Other.isEdgeSite())
    return getSiteKind() == Other.getSiteKind();
  return isEdgeSite() && Other.isEdgeSite() && Site == Other.Site;
}

void ProgramPoint::sort(ObservationComparator Compare) {
  if (Sorted && SortedWith == Compare)
    return;
  Observations.sort(Compare);
  Sorted = true;
  SortedWith = Compare;
}

ProgramPoint::iterator ProgramPoint::erase(iterator It) {
  assert(It != Observations.end() && "Cannot erase end iterator");
  --NumObservations;
  // Destroying the observation unlinks it from its memory object.
  return Observations.eraseAndDispose(It, std::default_delete<Observation>());
}

ProgramPoint::iterator ProgramPoint::erase(Observation &Obs) {
  assert(Obs.getParent() == this &&
         "observation does not belong to this program point");
  return erase(Obs.getIterator());
}

void ProgramPoint::mergeObservationsFrom(const ProgramPoint &Other) {
  assert(
      hasCompatibleMergeSemantics(Other) &&
      "cannot merge program points with incompatible site or guard semantics");
  for (const Observation &A : Other.Observations)
    addObservation(A);
}

void ProgramPoint::takeObservationsFrom(ProgramPoint &Other) {
  assert(
      hasCompatibleMergeSemantics(Other) &&
      "cannot merge program points with incompatible site or guard semantics");
  assert(getParent() && getParent() == Other.getParent() &&
         "observations can only move between program points of one graph");
  if (Other.empty())
    return;
  for (Observation &Obs : Other.Observations)
    Obs.setParent(this);
  Observations.splice(Observations.begin(), Other.Observations);
  NumObservations += Other.NumObservations;
  Other.NumObservations = 0;
  Sorted = false;
}

std::string ProgramPoint::toString() const { return printToString(*this); }

void ProgramPoint::print(raw_ostream &OS) const {
  OS << Name << ": ";
  if (const auto *LoopPP = dyn_cast<LoopProgramPoint>(this)) {
    OS << (LoopPP->isLoopPreheader() ? "loop-preheader=" : "loop-exit=");
    printValueName(OS, *LoopPP->getLoopGraph().getLoop()->getHeader());
    OS << ", ";
  }
  if (isEdgeSite() || !hasAlwaysGuard()) {
    OS << "site=";
    Site.print(OS);
    if (const auto *ExecutionGuard =
            std::get_if<AnyProvenanceExecutedGuard>(&Guard)) {
      OS << ", guard=";
      ExecutionGuard->print(OS);
      if (const BasicBlock *Reset = ExecutionGuard->getResetBlock()) {
        OS << ", reset=";
        printValueName(OS, *Reset);
      }
    }
    OS << ", ";
  }
  OS << "preds=[";
  printEdgeList(OS, Parents);
  OS << "], succs=[";
  printEdgeList(OS, Children);
  OS << "]";
  for (const Observation &A : Observations) {
    OS << "\n  ";
    A.print(OS);
  }
  OS << "\n";
}

MAIRGraph::MAIRGraph(MAIROptimizationConfig Config)
    : OptimizationConfig(Config),
      OwnedResources(std::make_unique<MAIRGraphResources>()),
      Resources(OwnedResources.get()) {}

void MAIRGraph::assertLive(StringRef Operation) const {
  if (!isLive())
    report_fatal_error(Twine("cannot ") + Operation + " a non-live MAIRGraph");
}

MAIRGraph::MAIRGraph(const Function &Func, MAIROptimizationConfig Config)
    : MAIRGraph(Config) {
  F = &Func;
}

MAIRGraph::MAIRGraph(Function &Func, MAIROptimizationConfig Config)
    : MAIRGraph(std::as_const(Func), Config) {
  MutableF = &Func;
}

MAIRGraph::MAIRGraph(MAIRGraph &ParentGraph, const Loop &Loop)
    : F(ParentGraph.F), MutableF(ParentGraph.MutableF),
      OptimizationConfig(ParentGraph.OptimizationConfig),
      Resources(ParentGraph.Resources), Parent(&ParentGraph), ScopeLoop(&Loop) {
}

MAIRGraph::MAIRGraph(MAIRGraph &&Other) noexcept { moveFrom(Other); }

MAIRGraph &MAIRGraph::operator=(MAIRGraph &&Other) noexcept {
  if (this != &Other)
    moveFrom(Other);
  return *this;
}

void MAIRGraph::moveFrom(MAIRGraph &Other) {
  if (Parent || Other.Parent)
    report_fatal_error("cannot move a MAIR loop graph outside its owner");
  clearStorage();
  F = Other.F;
  MutableF = Other.MutableF;
  GraphState = Other.GraphState;
  OptimizationConfig = Other.OptimizationConfig;
  GraphAllocator = std::move(Other.GraphAllocator);
  LoopPointAllocator = std::move(Other.LoopPointAllocator);
  OwnedResources = std::move(Other.OwnedResources);
  Resources = Other.Resources;
  Analyses = std::move(Other.Analyses);
  ScopeLoop = Other.ScopeLoop;
  ChildGraphs = std::move(Other.ChildGraphs);
  LoopPreheader = Other.LoopPreheader;
  LoopExits = std::move(Other.LoopExits);
  ForwardBarrier = Other.ForwardBarrier;
  BackwardBarrier = Other.BackwardBarrier;
  BBEntryRepresentatives = std::move(Other.BBEntryRepresentatives);
  BBExitRepresentatives = std::move(Other.BBExitRepresentatives);
  ProgramPoints = std::move(Other.ProgramPoints);
  NumProgramPoints = Other.NumProgramPoints;
  NumEdges = Other.NumEdges;
  // Program points and loop graphs refer back to the graph that owns them.
  for (ProgramPoint &PP : ProgramPoints)
    PP.setParent(this);
  for (auto &Child : ChildGraphs)
    Child->Parent = this;
  Other.F = nullptr;
  Other.MutableF = nullptr;
  Other.Resources = nullptr;
  Other.Analyses.clearReferences();
  Other.GraphState = State::MovedFrom;
}

MAIRGraph::~MAIRGraph() { clearStorage(); }

SmallVector<MAIRGraph *, 8> MAIRGraph::graphs() {
  assertLive("iterate");
  MAIRScopeTree Tree{this};
  return SmallVector<MAIRGraph *, 8>(depth_first(&Tree));
}

SmallVector<const MAIRGraph *, 8> MAIRGraph::graphs() const {
  SmallVector<MAIRGraph *, 8> Graphs = const_cast<MAIRGraph *>(this)->graphs();
  return SmallVector<const MAIRGraph *, 8>(Graphs.begin(), Graphs.end());
}

SmallVector<ProgramPoint *, 16> MAIRGraph::allProgramPoints() {
  SmallVector<ProgramPoint *, 16> Result;
  for (MAIRGraph *Graph : graphs())
    for (ProgramPoint &PP : *Graph)
      Result.push_back(&PP);
  return Result;
}

SmallVector<const ProgramPoint *, 16> MAIRGraph::allProgramPoints() const {
  SmallVector<ProgramPoint *, 16> Points =
      const_cast<MAIRGraph *>(this)->allProgramPoints();
  return SmallVector<const ProgramPoint *, 16>(Points.begin(), Points.end());
}

MAIRGraph *MAIRGraph::getLoopGraph(const Loop &Loop) {
  return const_cast<MAIRGraph *>(std::as_const(*this).getLoopGraph(Loop));
}

const MAIRGraph *MAIRGraph::getLoopGraph(const Loop &Loop) const {
  if (ScopeLoop == &Loop)
    return this;
  for (const MAIRGraph *Graph : getRoot().graphs())
    if (Graph->getLoop() == &Loop)
      return Graph;
  return nullptr;
}

bool MAIRGraph::containsBlock(const BasicBlock &BB) const {
  assertLive("query");
  return BB.getParent() == F &&
         (!hasLoopInfo() || getLoopInfo().getLoopFor(&BB) == ScopeLoop);
}

void MAIRGraph::markConsumed() {
  for (MAIRGraph *Graph : getRoot().graphs())
    Graph->GraphState = State::ConsumedByInstantiation;
}

template <class T, class... Args>
T &MAIRGraph::allocateNode(Args &&...ArgsPack) {
  if constexpr (std::is_same_v<T, LoopProgramPoint>)
    return *new (LoopPointAllocator.Allocate())
        T(std::forward<Args>(ArgsPack)...);
  else
    return *new (GraphAllocator.Allocate()) T(std::forward<Args>(ArgsPack)...);
}

std::string MAIRGraph::createProgramPointName() {
  return (Twine("pp") + Twine(Resources->NextProgramPointID++)).str();
}

ProgramPoint &MAIRGraph::addProgramPoint(ProgramPoint &PP) {
  ProgramPoints.push_back(PP);
  PP.setParent(this);
  ++NumProgramPoints;
  return PP;
}

MemoryObject &MAIRGraph::getOrCreateMemoryObject(const Value &Base) {
  const unsigned NextID = Resources->MemoryObjects.size();
  MemoryObject *&Object = Resources->MemoryObjects[&Base];
  if (!Object)
    Object = new (Resources->MemoryObjectAllocator.Allocate())
        MemoryObject(Base, NextID);
  return *Object;
}

void MAIRGraph::registerObservation(Observation &Obs) {
  const Value *Base = Obs.getBase();
  assert(Base && "cannot add a base-less observation to a MAIRGraph");
  assert(Obs.getParent() && Obs.getParent()->getParent() == this &&
         "an observation is registered with the graph of its program point");
  Obs.refreshOptimizationProperties();
  Obs.setMemoryObject(&getOrCreateMemoryObject(*Base));
}

ProgramPoint &MAIRGraph::createBBProgramPoint(const BasicBlock &BB,
                                              bool IsEntry) {
  assertLive("create a program point on");
  const Instruction *Anchor =
      IsEntry ? &*BB.getFirstNonPHIIt() : BB.getTerminator();
  assert(Anchor && "a basic-block program point must have an anchor");
  ProgramPoint &PP = addProgramPoint(allocateNode<ProgramPoint>(
      createProgramPointName(), *Anchor,
      IsEntry ? ProgramPoint::SiteKind::BasicBlockEntry
              : ProgramPoint::SiteKind::BasicBlockExit));
  (IsEntry ? BBEntryRepresentatives : BBExitRepresentatives)[&BB] = &PP;
  return PP;
}

ProgramPoint &MAIRGraph::createInstProgramPoint(const BasicBlock &BB,
                                                const Instruction &Anchor) {
  assertLive("create a program point on");
  assert(Anchor.getParent() == &BB &&
         "a memory-instruction program point anchor must belong to its block");
  auto &PP = addProgramPoint(allocateNode<ProgramPoint>(
      createProgramPointName(), Anchor, ProgramPoint::SiteKind::MemoryInst));
  const DataLayout &DL = Anchor.getModule()->getDataLayout();
  if (const auto *Transfer = dyn_cast<AnyMemTransferInst>(&Anchor)) {
    AbstractValue Length = getAccessSize(*Transfer->getLength());
    PP.addObservation(makeObservation(Anchor, *Transfer->getRawDest(), Length,
                                      AccessKind::Write, DL));
    PP.addObservation(makeObservation(Anchor, *Transfer->getRawSource(), Length,
                                      AccessKind::Read, DL));
  } else {
    PP.addObservation(Observation::fromInstruction(Anchor, DL));
  }
  return PP;
}

ProgramPoint &MAIRGraph::createProgramPoint(ProgramPointSite Site,
                                            ProgramPointGuard Guard) {
  assertLive("create a program point on");
  return addProgramPoint(allocateNode<ProgramPoint>(
      createProgramPointName(), std::move(Site), std::move(Guard)));
}

LoopProgramPoint &
MAIRGraph::createLoopProgramPoint(ProgramPointSite Site,
                                  LoopProgramPoint::BoundaryKind Kind,
                                  MAIRGraph &LoopGraph) {
  assertLive("create a loop program point on");
  auto &PP = allocateNode<LoopProgramPoint>(createProgramPointName(),
                                            std::move(Site), Kind, LoopGraph);
  addProgramPoint(PP);
  return PP;
}

ProgramPoint &MAIRGraph::createEdgeProgramPoint(const BasicBlock &From,
                                                const BasicBlock &To,
                                                ProgramPointGuard Guard) {
  assertLive("create a program point on");
  return createProgramPoint(ProgramPointSite::edge(From, To), std::move(Guard));
}

MAIRScalarExpr MAIRGraph::createExpr(MAIRScalarExpr::Kind Kind, Type &Ty,
                                     const APInt &Constant,
                                     const Value *LLVMValue,
                                     ArrayRef<MAIRScalarExpr> Operands) {
  assertLive("create an expression on");
  auto *Node = new (Resources->ScalarExprAllocator.Allocate())
      MAIRScalarExprNode(Resources->ScalarExprOwner, Kind, Ty, Constant,
                         LLVMValue, Operands);
  return MAIRScalarExpr(*Node);
}

MAIRScalarExpr MAIRGraph::createConstantExpr(Type &Ty, const APInt &Value) {
  return createExpr(MAIRScalarExpr::Kind::Constant, Ty, Value, nullptr, {});
}

MAIRScalarExpr MAIRGraph::createValueExpr(const Value &Value) {
  return createExpr(MAIRScalarExpr::Kind::LLVMValue, *Value.getType(),
                    APInt(1, 0), &Value, {});
}

MAIRScalarExpr MAIRGraph::createUnaryExpr(MAIRScalarExpr::Kind Kind,
                                          Type &ResultType,
                                          MAIRScalarExpr Operand) {
  return createExpr(Kind, ResultType, APInt(1, 0), nullptr, {Operand});
}

MAIRScalarExpr MAIRGraph::createBinaryExpr(MAIRScalarExpr::Kind Kind,
                                           Type &ResultType, MAIRScalarExpr LHS,
                                           MAIRScalarExpr RHS) {
  return createExpr(Kind, ResultType, APInt(1, 0), nullptr, {LHS, RHS});
}

bool MAIRGraph::ownsScalarExpr(MAIRScalarExpr Expr) const {
  assertLive("query");
  return Resources && Expr.getOwner() == &Resources->ScalarExprOwner;
}

MAIRGraph::iterator MAIRGraph::erase(ProgramPoint &PP) {
  assertLive("erase from");
  assert(PP.getParent() == this &&
         "ProgramPoint does not belong to this MAIRGraph");
  if (PP.isLoopProgramPoint())
    report_fatal_error("cannot erase a MAIR loop boundary");

  // Remove each peer's mirror entry directly. A self-edge occurs in both local
  // lists but contributes only once to the graph edge count.
  auto RemoveMirror = [](auto &Edges, const ProgramPoint &Neighbor,
                         PPEdgeKind Kind) {
    auto EdgeIt = findEdge(Edges, Neighbor, Kind);
    assert(EdgeIt != Edges.end() && "MAIR edge lists are not symmetric");
    Edges.erase(EdgeIt);
  };
  for (const ProgramPointEdge &Edge : PP.Children) {
    ProgramPoint &To = Edge.getNode();
    if (&To != &PP)
      RemoveMirror(To.Parents, PP, Edge.getKind());
  }
  for (const ProgramPointEdge &Edge : PP.Parents) {
    ProgramPoint &From = Edge.getNode();
    if (&From != &PP)
      RemoveMirror(From.Children, PP, Edge.getKind());
  }
  const unsigned SelfEdges =
      llvm::count_if(PP.Children, [&](const ProgramPointEdge &Edge) {
        return &Edge.getNode() == &PP;
      });
  NumEdges -= PP.Children.size() + PP.Parents.size() - SelfEdges;
  PP.Children.clear();
  PP.Parents.clear();

  for (const BasicBlock *BB : PP.RepresentedBlocks) {
    auto EntryIt = BBEntryRepresentatives.find(BB);
    if (EntryIt != BBEntryRepresentatives.end() && EntryIt->second == &PP)
      BBEntryRepresentatives.erase(EntryIt);
    auto ExitIt = BBExitRepresentatives.find(BB);
    if (ExitIt != BBExitRepresentatives.end() && ExitIt->second == &PP)
      BBExitRepresentatives.erase(ExitIt);
  }
  for (Observation &Obs : PP.Observations)
    Obs.clearMemoryObject();
  auto Next = ProgramPoints.erase(PP.getIterator());
  --NumProgramPoints;
  return Next;
}

void MAIRGraph::clearStorage() {
  ChildGraphs.clear();
  GraphAllocator.DestroyAll();
  LoopPointAllocator.DestroyAll();
  ProgramPoints.clear();
  if (OwnedResources) {
    Resources->ScalarExprAllocator.DestroyAll();
    Resources->MemoryObjectAllocator.DestroyAll();
    Resources->MemoryObjects.clear();
    Resources->NextProgramPointID = 0;
  }
  BBEntryRepresentatives.clear();
  BBExitRepresentatives.clear();
  NumProgramPoints = 0;
  NumEdges = 0;
  LoopPreheader = nullptr;
  LoopExits.clear();
}

void MAIRGraph::clear() {
  assertLive("clear");
  if (Parent)
    report_fatal_error("cannot clear a MAIR loop graph outside its owner");
  clearStorage();
}

ProgramPoint *MAIRGraph::find(StringRef Name) {
  return const_cast<ProgramPoint *>(std::as_const(*this).find(Name));
}

const ProgramPoint *MAIRGraph::find(StringRef Name) const {
  assertLive("find in");
  for (const ProgramPoint &PP : ProgramPoints)
    if (PP.getName() == Name)
      return &PP;
  return nullptr;
}

ProgramPoint &MAIRGraph::getBBEntryRepresentative(const BasicBlock &BB) {
  return const_cast<ProgramPoint &>(
      std::as_const(*this).getBBEntryRepresentative(BB));
}

const ProgramPoint &
MAIRGraph::getBBEntryRepresentative(const BasicBlock &BB) const {
  assertLive("query");
  const ProgramPoint *Representative = BBEntryRepresentatives.lookup(&BB);
  assert(Representative && "Seems the MAIRGraph is broken.");
  return *Representative;
}

ProgramPoint &MAIRGraph::getBBExitRepresentative(const BasicBlock &BB) {
  return const_cast<ProgramPoint &>(
      std::as_const(*this).getBBExitRepresentative(BB));
}

const ProgramPoint &
MAIRGraph::getBBExitRepresentative(const BasicBlock &BB) const {
  assertLive("query");
  const ProgramPoint *Representative = BBExitRepresentatives.lookup(&BB);
  assert(Representative && "Seems the MAIRGraph is broken.");
  return *Representative;
}

void MAIRGraph::replaceProgramPoint(ProgramPoint &From, ProgramPoint &Into) {
  assertLive("replace program points in");
  if (From.getParent() != this || Into.getParent() != this)
    report_fatal_error("replacement program points must belong to this graph");
  if (From.isLoopProgramPoint() || Into.isLoopProgramPoint())
    report_fatal_error("cannot replace a MAIR loop boundary");
  assert(Into.hasCompatibleMergeSemantics(From) &&
         "replacement program points have incompatible semantics");
  Into.RepresentedBlocks.insert_range(From.RepresentedBlocks);
  for (const BasicBlock *BB : From.RepresentedBlocks) {
    auto EntryIt = BBEntryRepresentatives.find(BB);
    if (EntryIt != BBEntryRepresentatives.end() && EntryIt->second == &From)
      EntryIt->second = &Into;
    auto ExitIt = BBExitRepresentatives.find(BB);
    if (ExitIt != BBExitRepresentatives.end() && ExitIt->second == &From)
      ExitIt->second = &Into;
  }
}

void MAIRGraph::contractProgramPoint(ProgramPoint &Into, ProgramPoint &From) {
  assertLive("contract program points in");
  if (Into.getParent() != this || From.getParent() != this)
    report_fatal_error("contracted program points must belong to this graph");
  if (From.isLoopProgramPoint() || Into.isLoopProgramPoint())
    report_fatal_error("cannot contract a MAIR loop boundary");
  assert(Into.hasCompatibleMergeSemantics(From) &&
         "contracted program points have incompatible semantics");
  assert(Into.hasChild(From, PPEdgeKind::PostDom) &&
         From.hasChild(Into, PPEdgeKind::Dom) &&
         "contraction requires mutual Dom/PostDom edges");

  Into.takeObservationsFrom(From);
  SmallVector<ProgramPointEdge, 2> Incoming(From.parents());
  SmallVector<ProgramPointEdge, 2> Outgoing(From.children());
  for (const ProgramPointEdge &Edge : Incoming) {
    ProgramPoint &Pred = Edge.getNode();
    if (&Pred != &Into)
      addEdge(Pred, Into, Edge.getKind(), Edge.getIteratedCycle());
  }
  for (const ProgramPointEdge &Edge : Outgoing) {
    ProgramPoint &Succ = Edge.getNode();
    if (&Succ != &Into)
      addEdge(Into, Succ, Edge.getKind(), Edge.getIteratedCycle());
  }

  replaceProgramPoint(From, Into);
  erase(From);
}

void MAIRGraph::addEdge(ProgramPoint &From, ProgramPoint &To, PPEdgeKind Kind,
                        const Cycle *Iterated) {
  assertLive("add an edge to");
  if (From.getParent() != this || To.getParent() != this)
    report_fatal_error("MAIRGraph edge endpoints must belong to this graph");
  assert((!Iterated || Kind == PPEdgeKind::PostDom) &&
         "only a post-dominance edge can cross an iteration");
  auto ChildIt = findEdge(From.Children, To, Kind);
  if (ChildIt == From.Children.end()) {
    From.Children.push_back(ProgramPointEdge(To, Kind, Iterated));
    To.Parents.push_back(ProgramPointEdge(From, Kind, Iterated));
    ++NumEdges;
    return;
  }
  // The edge exists. Keep the outer of the two cycles: an observation that is
  // invariant in it is invariant in the other too.
  if (!Iterated || (ChildIt->Iterated && ChildIt->Iterated->contains(Iterated)))
    return;
  assert((!ChildIt->Iterated || Iterated->contains(ChildIt->Iterated)) &&
         "cycles iterated along one edge must be nested");
  auto ParentIt = findEdge(To.Parents, From, Kind);
  assert(ParentIt != To.Parents.end() && "MAIR edge lists are not symmetric");
  ChildIt->Iterated = Iterated;
  ParentIt->Iterated = Iterated;
}

void MAIRGraph::removeEdge(ProgramPoint &From, ProgramPoint &To,
                           PPEdgeKind Kind) {
  assertLive("remove an edge from");
  assert(From.getParent() == this && To.getParent() == this &&
         "MAIRGraph edge endpoints must belong to this graph");
  auto ChildIt = findEdge(From.Children, To, Kind);
  if (ChildIt == From.Children.end())
    return;
  From.Children.erase(ChildIt);

  auto ParentIt = findEdge(To.Parents, From, Kind);
  assert(ParentIt != To.Parents.end() && "MAIR edge lists are not symmetric");
  To.Parents.erase(ParentIt);
  --NumEdges;
}

bool MAIRGraph::hasEdge(const ProgramPoint &From, const ProgramPoint &To,
                        PPEdgeKind Kind) const {
  assertLive("query");
  return From.getParent() == this && To.getParent() == this &&
         From.hasChild(To, Kind);
}

bool MAIRGraph::isAncestor(const ProgramPoint &Ancestor,
                           const ProgramPoint &Descendant,
                           PPEdgeKind Kind) const {
  assertLive("query ancestry in");
  if (Ancestor.getParent() != this || Descendant.getParent() != this)
    return false;
  if (&Ancestor == &Descendant)
    return false;

  SmallVector<const ProgramPoint *, 8> Worklist{&Ancestor};
  SmallPtrSet<const ProgramPoint *, 16> Visited{&Ancestor};
  while (!Worklist.empty()) {
    const ProgramPoint &Current = *Worklist.pop_back_val();
    for (const ProgramPointEdge &Edge : Current.children()) {
      if (Edge.getKind() != Kind)
        continue;
      const ProgramPoint *Next = &Edge.getNode();
      if (Next == &Descendant)
        return true;
      if (Visited.insert(Next).second)
        Worklist.push_back(Next);
    }
  }
  return false;
}

MemoryObject *MAIRGraph::getMemoryObject(const Value &Base) {
  assertLive("query");
  return Resources->MemoryObjects.lookup(&Base);
}

const MemoryObject *MAIRGraph::getMemoryObject(const Value &Base) const {
  assertLive("query");
  return Resources->MemoryObjects.lookup(&Base);
}

std::string MAIRGraph::toString() const { return printToString(*this); }

void MAIRGraph::print(raw_ostream &OS) const {
  assertLive("print");
  OS << "mair graph";
  if (F) {
    OS << " of fn ";
    printValueName(OS, *F);
  }
  if (ScopeLoop) {
    OS << " of loop ";
    printValueName(OS, *ScopeLoop->getHeader());
  }
  OS << " {\n";
  for (const ProgramPoint &PP : ProgramPoints) {
    PP.print(OS);
    OS << "\n";
  }
  OS << "}\n\n";
  for (const auto &Child : ChildGraphs)
    Child->print(OS);
}

} // namespace mair
