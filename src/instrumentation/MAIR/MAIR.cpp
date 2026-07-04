#include "MAIR/MAIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace mair {

namespace {
const Instruction *getBarrierRangeStart(const Observation &Obs) {
  return Obs.isBasicBlockEntrySite() ? nullptr : Obs.getAnchor();
}

const Instruction *getBarrierRangeEnd(const Observation &Obs) {
  return Obs.isBasicBlockExitSite() ? nullptr : Obs.getAnchor();
}

SmallVector<BasicBlock *, 16> collectReachableBlocks(Function &F) {
  SmallVector<BasicBlock *, 16> Blocks;
  SmallVector<BasicBlock *, 16> Worklist;
  SmallPtrSet<BasicBlock *, 16> Seen;

  Worklist.push_back(&F.getEntryBlock());
  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.pop_back_val();
    if (!Seen.insert(BB).second)
      continue;
    Blocks.push_back(BB);
    const Instruction *Term = BB->getTerminator();
    for (unsigned I = 0, E = Term->getNumSuccessors(); I != E; ++I)
      Worklist.push_back(Term->getSuccessor(I));
  }
  return Blocks;
}

bool isInterestingMop(const Instruction &I) {
  if (I.hasMetadata(LLVMContext::MD_nosanitize))
    return false;
  if (I.isAtomic())
    return false;
  return isa<LoadInst>(I) || isa<StoreInst>(I);
}
} // namespace

// Build the initial, unoptimized MAIR graph for one LLVM function.
//
// Observation layout follows each LLVM basic block:
//   BBEntry -> memory observations in source order -> BBExit.
// Edges are then added for adjacent observations inside each block and for
// immediate dominator/post-dominator relationships across blocks. Barrier
// queries decide whether the corresponding forward/backward sanitizer
// reachability edge is legal.
MAIRGraph MAIRGraph::build(Function &F, const DominatorTree &DT,
                           const PostDominatorTree &PDT,
                           MAIRBarrierAnalysisCore &BA) {
  MAIRGraph Graph(F);
  DenseMap<const BasicBlock *, Observation *> BBEntryObs;
  DenseMap<const BasicBlock *, Observation *> BBExitObs;
  DenseMap<const BasicBlock *, SmallVector<Observation *, 4>> OrderedObs;

  // ===========================================================
  // 1. Create observations for each MA and BB
  // ===========================================================
  for (BasicBlock &BB : F) {
    auto &ObsInOneBB = OrderedObs[&BB];
    auto &Start = Graph.createBBObservation(BB, true);
    BBEntryObs[&BB] = &Start;
    ObsInOneBB.push_back(&Start);
    for (Instruction &I : BB) {
      if (isInterestingMop(I)) {
        auto &Obs = Graph.createInstObservation(BB, I);
        ObsInOneBB.push_back(&Obs);
      }
    }
    auto &End = Graph.createBBObservation(BB, false);
    BBExitObs[&BB] = &End;
    ObsInOneBB.push_back(&End);
  }

  // ===========================================================
  // 2. Connect observations' edges intra-block
  // ===========================================================
  for (auto &Entry : OrderedObs) {
    const BasicBlock &BB = *Entry.first;
    ArrayRef<Observation *> Points = Entry.second;
    assert(Points.size() > 1);
    for (unsigned I = 1; I < Points.size(); ++I) {
      Observation &Prev = *Points[I - 1], &Next = *Points[I];
      const Instruction *PrevI = getBarrierRangeStart(Prev);
      const Instruction *NextI = getBarrierRangeEnd(Next);
      if (!BA.hasFwdBarrierBetween(BB, PrevI, NextI))
        Graph.addEdge(Prev, Next);
      if (!BA.hasBwdBarrierBetween(BB, PrevI, NextI))
        Graph.addEdge(Next, Prev);
    }
  }

  // ===========================================================
  // 3. Connect observations' edges inter-block via DT/PDT
  // ===========================================================
  auto GetIDom = [](auto &DT, const BasicBlock &BB) -> const BasicBlock * {
    if (const DomTreeNode *Node = DT.getNode(&BB))
      if (const DomTreeNode *IDom = Node->getIDom())
        return IDom->getBlock();
    return nullptr;
  };
  for (BasicBlock &BB : F) {
    Observation *EntryObs = BBEntryObs.lookup(&BB);
    Observation *ExitObs = BBExitObs.lookup(&BB);
    assert(EntryObs && ExitObs);
    if (const BasicBlock *IDom = GetIDom(DT, BB)) {
      Observation *Pred = BBEntryObs.lookup(IDom);
      assert(Pred);
      if (!BA.hasFwdBarrierPath(*IDom, BB))
        Graph.addEdge(*Pred, *EntryObs);
    }
    if (const BasicBlock *IPDom = GetIDom(PDT, BB)) {
      Observation *Succ = BBEntryObs.lookup(IPDom);
      assert(Succ);
      if (!BA.hasBwdBarrierPath(BB, *IPDom))
        Graph.addEdge(*ExitObs, *Succ);
    }
  }

  return Graph;
}

namespace {
static AccessKind mergeKind(AccessKind LHS, AccessKind RHS) {
  if (LHS == RHS)
    return LHS;
  return AccessKind::ReadWrite;
}

static std::string getPrintedValueName(const Value &V) {
  std::string Name;
  raw_string_ostream OS(Name);
  V.printAsOperand(OS, false);
  OS.flush();
  return Name;
}

static void printAccessKind(raw_ostream &OS, AccessKind Kind) {
  switch (Kind) {
  case AccessKind::Read:
    OS << "read";
    break;
  case AccessKind::Write:
    OS << "write";
    break;
  case AccessKind::ReadWrite:
    OS << "read/write";
    break;
  }
}

static void printAccessSize(raw_ostream &OS, LocationSize Size) {
  if (!Size.hasValue()) {
    OS << "unknown";
    return;
  }
  if (!Size.isPrecise())
    OS << "<=";
  OS << Size.getValue();
}

} // namespace

void MemoryAccess::mergeKind(AccessKind Other) {
  Kind = ::mair::mergeKind(Kind, Other);
}

std::string MemoryAccess::toString(bool LLVM) const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS, LLVM);
  OS.flush();
  return S;
}

MemoryAccess MemoryAccess::fromInstruction(const Instruction &I,
                                           const DataLayout &DL) {
  (void)DL;
  std::string InstText;
  raw_string_ostream InstOS(InstText);
  I.print(InstOS);
  InstOS.flush();

  StringRef Text = StringRef(InstText).ltrim();
  AccessKind Kind = AccessKind::Read;
  if (Text.contains(" load "))
    Kind = AccessKind::Read;
  else if (Text.starts_with("store "))
    Kind = AccessKind::Write;
  else if (I.mayReadFromMemory() && I.mayWriteToMemory())
    Kind = AccessKind::ReadWrite;
  else if (I.mayWriteToMemory())
    Kind = AccessKind::Write;
  else if (!I.mayReadFromMemory())
    report_fatal_error(
        "MemoryAccess::fromInstruction expects a memory access instruction");

  MemoryLocation Loc = MemoryLocation::get(&I);
  return MemoryAccess(&I, Loc, Kind);
}

void MemoryAccess::print(raw_ostream &OS, bool LLVM) const {
  (void)LLVM;
  printAccessKind(OS, Kind);
  OS << " ptr ";
  if (Loc.Ptr)
    OS << getPrintedValueName(*Loc.Ptr);
  else
    OS << "<unknown>";
  OS << ", ";
  printAccessSize(OS, Loc.Size);
}

Observation::Observation(StringRef N, const BasicBlock *Block,
                         const Instruction *Anchor, SiteKind K)
    : Name(N.str()), BB(Block), AnchorInst(Anchor), Kind(K) {}

Observation::~Observation() { AccessAllocator.DestroyAll(); }

MemoryAccess &Observation::createAccess(const Instruction *I,
                                        MemoryLocation Loc, AccessKind Kind) {
  return *new (AccessAllocator.Allocate()) MemoryAccess(I, Loc, Kind);
}

MemoryAccess &Observation::addAccess(const MemoryAccess &A) {
  auto &Node = createAccess(A.getInstruction(), A.getLocation(), A.getKind());
  Accesses.push_back(Node);
  Node.setParent(this);
  ++NumAccesses;
  return Node;
}

Observation::iterator Observation::erase(iterator It) {
  assert(It != Accesses.end() && "Cannot erase end iterator");
  auto Next = Accesses.erase(It);
  --NumAccesses;
  return Next;
}

void Observation::mergeAccessesFrom(const Observation &Other) {
  for (const MemoryAccess &A : Other.Accesses)
    addAccess(A);
}

std::string Observation::toString(bool LLVM) const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS, LLVM);
  OS.flush();
  return S;
}

void Observation::print(raw_ostream &OS, bool LLVM) const {
  (void)LLVM;
  OS << Name << ": preds=[";
  if (const MAIRGraph *Graph = getParent()) {
    MAIRGraph::ConstEdgeListType Preds = Graph->predecessors(*this);
    bool First = true;
    for (const Observation *Pred : Preds) {
      if (!First)
        OS << ", ";
      OS << Pred->getName();
      First = false;
    }
  }
  OS << "], succs=[";
  if (const MAIRGraph *Graph = getParent()) {
    MAIRGraph::ConstEdgeListType Succs = Graph->successors(*this);
    bool First = true;
    for (const Observation *Succ : Succs) {
      if (!First)
        OS << ", ";
      OS << Succ->getName();
      First = false;
    }
  }
  OS << "]";
  for (const MemoryAccess &A : Accesses) {
    OS << "\n  ";
    A.print(OS, LLVM);
  }
}

MAIRGraph::MAIRGraph(const Function &Func) : F(&Func) {}

MAIRGraph::MAIRGraph(MAIRGraph &&Other) noexcept
    : F(Other.F), GraphAllocator(std::move(Other.GraphAllocator)),
      Observations(std::move(Other.Observations)),
      Edges(std::move(Other.Edges)), SuccMap(std::move(Other.SuccMap)),
      PredMap(std::move(Other.PredMap)), NumObservations(Other.NumObservations),
      NumEdges(Other.NumEdges), NextObservationID(Other.NextObservationID) {
  for (Observation &Obs : Observations)
    Obs.setParent(this);
}

MAIRGraph &MAIRGraph::operator=(MAIRGraph &&Other) noexcept {
  if (this == &Other)
    return *this;
  clear();
  F = Other.F;
  GraphAllocator = std::move(Other.GraphAllocator);
  Observations = std::move(Other.Observations);
  Edges = std::move(Other.Edges);
  SuccMap = std::move(Other.SuccMap);
  PredMap = std::move(Other.PredMap);
  NumObservations = Other.NumObservations;
  NumEdges = Other.NumEdges;
  NextObservationID = Other.NextObservationID;
  for (Observation &Obs : Observations)
    Obs.setParent(this);
  return *this;
}

MAIRGraph::~MAIRGraph() { clear(); }

template <class T, class... Args>
T &MAIRGraph::allocateNode(Args &&...ArgsPack) {
  return *new (GraphAllocator.Allocate()) T(std::forward<Args>(ArgsPack)...);
}

std::string MAIRGraph::createObservationName(StringRef Prefix) {
  return (Twine(Prefix) + Twine(NextObservationID++)).str();
}

Observation &MAIRGraph::addObservation(Observation &Obs) {
  Observations.push_back(Obs);
  Obs.setParent(this);
  ++NumObservations;
  return Obs;
}

Observation &MAIRGraph::createBBObservation(const BasicBlock &BB,
                                            bool IsEntry) {
  const Instruction *Anchor =
      IsEntry ? &*BB.getFirstNonPHIIt() : BB.getTerminator();
  return addObservation(allocateNode<Observation>(
      createObservationName(IsEntry ? "bs" : "be"), &BB, Anchor,
      IsEntry ? Observation::SiteKind::BasicBlockEntry
              : Observation::SiteKind::BasicBlockExit));
}

Observation &MAIRGraph::createInstObservation(const BasicBlock &BB,
                                              const Instruction &Anchor) {
  auto &Obs = addObservation(
      allocateNode<Observation>(createObservationName("ma"), &BB, &Anchor,
                                Observation::SiteKind::MemoryInst));
  Obs.addAccess(MemoryAccess::fromInstruction(
      Anchor, Anchor.getModule()->getDataLayout()));
  return Obs;
}

MAIRGraph::iterator MAIRGraph::erase(Observation &Obs) {
  auto It = llvm::find_if(Observations, [&Obs](Observation &Candidate) {
    return &Candidate == &Obs;
  });
  assert(It != Observations.end() &&
         "Observation does not belong to this MAIRGraph");
  SmallVector<Observation *, 4> Succs = successors(Obs);
  SmallVector<Observation *, 4> Preds = predecessors(Obs);
  for (Observation *Succ : Succs)
    removeEdge(Obs, *Succ);
  for (Observation *Pred : Preds)
    removeEdge(*Pred, Obs);
  auto Next = Observations.erase(It);
  SuccMap.erase(&Obs);
  PredMap.erase(&Obs);
  --NumObservations;
  return Next;
}

void MAIRGraph::clear() {
  GraphAllocator.DestroyAll();
  Observations.clear();
  Edges.clear();
  SuccMap.clear();
  PredMap.clear();
  NumObservations = 0;
  NumEdges = 0;
  NextObservationID = 0;
}

Observation *MAIRGraph::find(StringRef Name) {
  return const_cast<Observation *>(std::as_const(*this).find(Name));
}

const Observation *MAIRGraph::find(StringRef Name) const {
  for (const Observation &Obs : Observations)
    if (Obs.getName() == Name)
      return &Obs;
  return nullptr;
}

void MAIRGraph::addEdge(Observation &From, Observation &To) {
  for (const auto &Edge : Edges)
    if (Edge.first == &From && Edge.second == &To)
      return;
  Edges.push_back({&From, &To});
  SuccMap[&From].push_back(&To);
  PredMap[&To].push_back(&From);
  ++NumEdges;
}

void MAIRGraph::removeEdge(Observation &From, Observation &To) {
  auto It = llvm::find_if(Edges, [&From, &To](const auto &Edge) {
    return Edge.first == &From && Edge.second == &To;
  });
  if (It == Edges.end())
    return;
  Edges.erase(It);
  if (auto Succs = SuccMap.find(&From); Succs != SuccMap.end())
    Succs->second.erase(
        std::remove(Succs->second.begin(), Succs->second.end(), &To),
        Succs->second.end());
  if (auto Preds = PredMap.find(&To); Preds != PredMap.end())
    Preds->second.erase(
        std::remove(Preds->second.begin(), Preds->second.end(), &From),
        Preds->second.end());
  --NumEdges;
}

MAIRGraph::EdgeListType MAIRGraph::successors(Observation &Obs) {
  EdgeListType Result;
  if (auto Succs = SuccMap.find(&Obs); Succs != SuccMap.end())
    Result.append(Succs->second.begin(), Succs->second.end());
  return Result;
}

MAIRGraph::ConstEdgeListType
MAIRGraph::successors(const Observation &Obs) const {
  ConstEdgeListType Result;
  if (auto Succs = SuccMap.find(const_cast<Observation *>(&Obs));
      Succs != SuccMap.end())
    Result.append(Succs->second.begin(), Succs->second.end());
  return Result;
}

MAIRGraph::EdgeListType MAIRGraph::predecessors(Observation &Obs) {
  EdgeListType Result;
  if (auto Preds = PredMap.find(&Obs); Preds != PredMap.end())
    Result.append(Preds->second.begin(), Preds->second.end());
  return Result;
}

MAIRGraph::ConstEdgeListType
MAIRGraph::predecessors(const Observation &Obs) const {
  ConstEdgeListType Result;
  if (auto Preds = PredMap.find(const_cast<Observation *>(&Obs));
      Preds != PredMap.end())
    Result.append(Preds->second.begin(), Preds->second.end());
  return Result;
}

std::string MAIRGraph::toString(bool LLVM) const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS, LLVM);
  OS.flush();
  return S;
}

void MAIRGraph::print(raw_ostream &OS, bool LLVM) const {
  (void)LLVM;
  OS << "mair graph";
  if (F) {
    OS << " of fn ";
    OS << getPrintedValueName(*F);
  }
  OS << " {\n";
  for (const Observation &Obs : Observations) {
    Obs.print(OS, LLVM);
    OS << "\n";
  }
  OS << "}\n";
}

} // namespace mair
