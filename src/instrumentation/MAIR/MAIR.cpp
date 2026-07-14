#include "MAIR/MAIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using namespace llvm;

namespace mair {

struct MAIRScalarExprOwner {};

struct MAIRScalarExprNode {
  const MAIRScalarExprOwner *Owner = nullptr;
  MAIRScalarExpr::Kind Kind = MAIRScalarExpr::Kind::Constant;
  Type *ResultType = nullptr;
  APInt Constant = APInt(1, 0);
  const Value *LLVMValue = nullptr;
  SmallVector<MAIRScalarExpr, 2> Operands;

  MAIRScalarExprNode(const MAIRScalarExprOwner &O, MAIRScalarExpr::Kind K,
                     Type &Ty, const APInt &C, const Value *V,
                     ArrayRef<MAIRScalarExpr> Ops)
      : Owner(&O), Kind(K), ResultType(&Ty), Constant(C), LLVMValue(V),
        Operands(Ops) {}
};

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
        Graph.addEdge(Prev, Next, ObsEdgeKind::Dom);
      if (!BA.hasBwdBarrierBetween(BB, PrevI, NextI))
        Graph.addEdge(Next, Prev, ObsEdgeKind::PostDom);
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
        Graph.addEdge(*Pred, *EntryObs, ObsEdgeKind::Dom);
    }
    if (const BasicBlock *IPDom = GetIDom(PDT, BB)) {
      Observation *Succ = BBEntryObs.lookup(IPDom);
      assert(Succ);
      if (!BA.hasBwdBarrierPath(BB, *IPDom))
        Graph.addEdge(*Succ, *ExitObs, ObsEdgeKind::PostDom);
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

static std::optional<uint64_t> getNonNegativeU64(const APInt &Value) {
  if (Value.isNegative())
    return std::nullopt;
  if (Value.getActiveBits() > 64)
    return std::nullopt;
  return Value.getZExtValue();
}

static std::optional<uint64_t> checkedEnd(uint64_t Begin, uint64_t Size) {
  if (UINT64_MAX - Begin < Size)
    return std::nullopt;
  return Begin + Size;
}

static AbstractValue getStoreSize(Type &Ty, const DataLayout &DL) {
  TypeSize Size = DL.getTypeStoreSize(&Ty);
  if (Size.isScalable())
    return AbstractValue::unknown();
  return AbstractValue::constant(Size.getFixedValue());
}

static void printAbstractValue(raw_ostream &OS, const AbstractValue &Value) {
  if (Value.isUnknown()) {
    OS << "unknown";
    return;
  }
  if (Value.isConstant()) {
    OS << Value.getConstant();
    return;
  }
  OS << getPrintedValueName(*Value.getSymbol());
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

static void printEdgeKind(raw_ostream &OS, ObsEdgeKind Kind) {
  switch (Kind) {
  case ObsEdgeKind::Dom:
    OS << "dom";
    break;
  case ObsEdgeKind::PostDom:
    OS << "pdom";
    break;
  }
}

static void printEdgeList(raw_ostream &OS, ArrayRef<ObservationEdge> Edges) {
  bool First = true;
  for (const ObservationEdge &Edge : Edges) {
    if (!First)
      OS << ", ";
    OS << Edge.getNode().getName() << "(";
    printEdgeKind(OS, Edge.getKind());
    OS << ")";
    First = false;
  }
}

} // namespace

ObservationSite ObservationSite::anchored(const BasicBlock *BB,
                                          const Instruction *Anchor, Kind K) {
  assert(K != Kind::Edge && "an anchored site cannot have edge kind");
  ObservationSite Site;
  Site.SiteKind = K;
  Site.BB = BB;
  Site.Anchor = Anchor;
  return Site;
}

ObservationSite ObservationSite::edge(const BasicBlock &From,
                                      const BasicBlock &To) {
  ObservationSite Site;
  Site.SiteKind = Kind::Edge;
  Site.From = &From;
  Site.To = &To;
  return Site;
}

void ObservationSite::print(raw_ostream &OS) const {
  if (!isEdge()) {
    switch (SiteKind) {
    case Kind::MemoryInst:
      OS << "memory(";
      break;
    case Kind::BasicBlockEntry:
      OS << "bb.entry(";
      break;
    case Kind::BasicBlockExit:
      OS << "bb.exit(";
      break;
    case Kind::Edge:
      llvm_unreachable("handled edge observation site");
    }
    if (BB)
      BB->printAsOperand(OS, false);
    else
      OS << "null";
    if (SiteKind == Kind::MemoryInst) {
      OS << ", ";
      if (Anchor)
        Anchor->printAsOperand(OS, false);
      else
        OS << "null";
    }
    OS << ")";
    return;
  }
  OS << "edge(";
  if (From)
    From->printAsOperand(OS, false);
  else
    OS << "null";
  OS << " -> ";
  if (To)
    To->printAsOperand(OS, false);
  else
    OS << "null";
  OS << ")";
}

bool AnyProvenanceExecutedGuard::operator==(
    const AnyProvenanceExecutedGuard &Other) const {
  if (Provenance.size() != Other.Provenance.size())
    return false;
  return llvm::all_of(Provenance, [&](const Instruction *I) {
    return llvm::is_contained(Other.Provenance, I);
  });
}

MAIRScalarExpr::Kind MAIRScalarExpr::getKind() const {
  assert(Node && "invalid MAIR scalar expression");
  return Node->Kind;
}

Type *MAIRScalarExpr::getType() const {
  assert(Node && "invalid MAIR scalar expression");
  return Node->ResultType;
}

const APInt &MAIRScalarExpr::getConstant() const {
  assert(Node && Node->Kind == Kind::Constant &&
         "MAIR scalar expression is not a constant");
  return Node->Constant;
}

const Value *MAIRScalarExpr::getLLVMValue() const {
  assert(Node && Node->Kind == Kind::LLVMValue &&
         "MAIR scalar expression is not an LLVM value");
  return Node->LLVMValue;
}

ArrayRef<MAIRScalarExpr> MAIRScalarExpr::operands() const {
  assert(Node && "invalid MAIR scalar expression");
  return Node->Operands;
}

const MAIRScalarExprOwner *MAIRScalarExpr::getOwner() const {
  return Node ? Node->Owner : nullptr;
}

bool MAIRScalarExpr::operator==(const MAIRScalarExpr &Other) const {
  if (Node == Other.Node)
    return true;
  if (!Node || !Other.Node || Node->Kind != Other.Node->Kind ||
      Node->ResultType != Other.Node->ResultType ||
      Node->LLVMValue != Other.Node->LLVMValue ||
      Node->Operands.size() != Other.Node->Operands.size())
    return false;
  if (Node->Kind == Kind::Constant && Node->Constant != Other.Node->Constant)
    return false;
  for (size_t I = 0; I < Node->Operands.size(); ++I)
    if (Node->Operands[I] != Other.Node->Operands[I])
      return false;
  return true;
}

void MAIRScalarExpr::print(raw_ostream &OS, bool Signed) const {
  if (!Node) {
    OS << "<invalid>";
    return;
  }
  if (Node->Kind == Kind::Constant) {
    SmallString<32> Text;
    Node->Constant.toString(Text, 10, Signed);
    OS << Text;
    return;
  }
  if (Node->Kind == Kind::LLVMValue) {
    if (Node->LLVMValue)
      OS << getPrintedValueName(*Node->LLVMValue);
    else
      OS << "<null>";
    return;
  }

  auto PrintBinary = [&](StringRef Operator) {
    OS << "(";
    if (!Node->Operands.empty())
      Node->Operands[0].print(OS, Signed);
    OS << " " << Operator << " ";
    if (Node->Operands.size() > 1)
      Node->Operands[1].print(OS, Signed);
    OS << ")";
  };
  switch (Node->Kind) {
  case Kind::Add:
    return PrintBinary("+");
  case Kind::Mul:
    return PrintBinary("*");
  case Kind::UDiv:
    return PrintBinary("udiv");
  case Kind::SMin:
    return PrintBinary("smin");
  case Kind::UMin:
    return PrintBinary("umin");
  case Kind::SMax:
    return PrintBinary("smax");
  case Kind::UMax:
    return PrintBinary("umax");
  case Kind::Truncate:
    OS << "trunc(";
    break;
  case Kind::ZeroExtend:
    OS << "zext(";
    break;
  case Kind::SignExtend:
    OS << "sext(";
    break;
  case Kind::Constant:
  case Kind::LLVMValue:
    llvm_unreachable("handled scalar expression leaf");
  }
  if (!Node->Operands.empty())
    Node->Operands.front().print(OS, Signed);
  OS << " to ";
  if (Node->ResultType)
    Node->ResultType->print(OS);
  else
    OS << "<null>";
  OS << ")";
}

void PeriodicRegion::print(raw_ostream &OS) const {
  OS << "period start=";
  Start.print(OS);
  OS << ", stride=";
  Stride.print(OS, true);
  OS << ", access-size=";
  AccessSize.print(OS);
  OS << ", trips=";
  TripCount.print(OS);
}

void MemoryUse::set(MemoryObject *NewObject) {
  if (Object == NewObject)
    return;
  removeFromList();
  Object = NewObject;
  if (Object)
    addToList(&Object->UseList);
}

void MemoryUse::addToList(MemoryUse **List) {
  assert(!Prev && !Next && "MAIR memory use is already linked");
  Next = *List;
  if (Next)
    Next->Prev = &Next;
  Prev = List;
  *Prev = this;
}

void MemoryUse::removeFromList() {
  if (!Prev)
    return;
  *Prev = Next;
  if (Next)
    Next->Prev = Prev;
  Object = nullptr;
  Next = nullptr;
  Prev = nullptr;
}

AbstractValue::Kind AbstractValue::getKind() const {
  if (isUnknown())
    return Kind::Unknown;
  if (isSymbol())
    return Kind::Symbol;
  return Kind::Constant;
}

AbstractValue AbstractValue::symbol(const Value &V) {
  AbstractValue Result;
  Result.Storage = &V;
  return Result;
}

AbstractValue AbstractValue::constant(uint64_t C) {
  AbstractValue Result;
  Result.Storage = C;
  return Result;
}

AbstractAccess AbstractAccess::fromPointerAndSize(const Value &Ptr,
                                                  AbstractValue Size,
                                                  const DataLayout &DL) {
  APInt Offset(DL.getIndexTypeSizeInBits(Ptr.getType()), 0);
  const Value *Base = Ptr.stripAndAccumulateConstantOffsets(
      DL, Offset, /*AllowNonInbounds=*/true);
  const Value *Underlying = getUnderlyingObject(&Ptr);
  std::optional<uint64_t> ConcreteOffset = getNonNegativeU64(Offset);
  if (Base && ConcreteOffset && (Base != &Ptr || Underlying == &Ptr))
    return AbstractAccess(Base, AbstractValue::constant(*ConcreteOffset), Size);

  if (Underlying && Underlying != &Ptr)
    return AbstractAccess(Underlying, AbstractValue::symbol(Ptr), Size);

  return AbstractAccess(&Ptr, AbstractValue::constant(0), Size);
}

bool AbstractAccess::contains(const AbstractAccess &Other) const {
  if (*this == Other)
    return true;
  if (Base != Other.Base)
    return false;
  if (!Offset.isConstant() || !Size.isConstant() ||
      !Other.Offset.isConstant() || !Other.Size.isConstant())
    return false;

  uint64_t ThisBegin = Offset.getConstant();
  std::optional<uint64_t> ThisEnd = checkedEnd(ThisBegin, Size.getConstant());
  uint64_t OtherBegin = Other.Offset.getConstant();
  std::optional<uint64_t> OtherEnd =
      checkedEnd(OtherBegin, Other.Size.getConstant());
  if (!ThisEnd || !OtherEnd)
    return false;
  return ThisBegin <= OtherBegin && *OtherEnd <= *ThisEnd;
}

std::optional<AbstractAccess> AbstractAccess::merge(const AbstractAccess &A,
                                                    const AbstractAccess &B) {
  if (A == B)
    return A;
  if (A.Base != B.Base)
    return std::nullopt;
  if (A.contains(B))
    return A;
  if (B.contains(A))
    return B;
  if (!A.Offset.isConstant() || !A.Size.isConstant() ||
      !B.Offset.isConstant() || !B.Size.isConstant())
    return std::nullopt;

  uint64_t ABegin = A.Offset.getConstant();
  std::optional<uint64_t> AEnd = checkedEnd(ABegin, A.Size.getConstant());
  uint64_t BBegin = B.Offset.getConstant();
  std::optional<uint64_t> BEnd = checkedEnd(BBegin, B.Size.getConstant());
  if (!AEnd || !BEnd)
    return std::nullopt;
  if (*AEnd < BBegin || *BEnd < ABegin)
    return std::nullopt;

  uint64_t Begin = std::min(ABegin, BBegin);
  uint64_t End = std::max(*AEnd, *BEnd);

  return AbstractAccess(A.Base, AbstractValue::constant(Begin),
                        AbstractValue::constant(End - Begin));
}

void AbstractAccess::print(raw_ostream &OS) const {
  if (!Base && Offset.isUnknown()) {
    OS << "<unknown>";
    return;
  }
  if (Base)
    OS << getPrintedValueName(*Base);
  else
    OS << "unknown";
  if (Offset.isConstant()) {
    if (Offset.getConstant() != 0)
      OS << " + " << Offset.getConstant();
  } else {
    OS << " + ";
    printAbstractValue(OS, Offset);
  }
}

void MemoryAccess::mergeKind(AccessKind Other) {
  Kind = ::mair::mergeKind(Kind, Other);
}

const Value *MemoryAccess::getBase() const {
  return std::visit([](const auto &R) { return R.getBase(); }, Region);
}

bool MemoryAccess::mergeWith(const MemoryAccess &Other) {
  if (const auto *ThisAccess = getContiguousRegion()) {
    const auto *OtherAccess = Other.getContiguousRegion();
    if (!OtherAccess)
      return false;
    std::optional<AbstractAccess> Merged =
        AbstractAccess::merge(*ThisAccess, *OtherAccess);
    if (!Merged)
      return false;
    Region = *Merged;
  } else {
    const auto *ThisPeriodic = getPeriodicRegion();
    const auto *OtherPeriodic = Other.getPeriodicRegion();
    if (!ThisPeriodic || !OtherPeriodic || *ThisPeriodic != *OtherPeriodic)
      return false;
  }

  if (getBase() != Other.getBase())
    return false;
  Insts.append(Other.Insts);
  mergeKind(Other.Kind);
  return true;
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
  const Value *Ptr = nullptr;
  AbstractValue Size = AbstractValue::unknown();
  AccessKind Kind = AccessKind::Read;
  if (const auto *LI = dyn_cast<LoadInst>(&I)) {
    Ptr = LI->getPointerOperand();
    Size = getStoreSize(*LI->getType(), DL);
    Kind = AccessKind::Read;
  } else if (const auto *SI = dyn_cast<StoreInst>(&I)) {
    Ptr = SI->getPointerOperand();
    Size = getStoreSize(*SI->getValueOperand()->getType(), DL);
    Kind = AccessKind::Write;
  } else if (I.mayReadFromMemory() && I.mayWriteToMemory()) {
    Kind = AccessKind::ReadWrite;
  } else if (I.mayWriteToMemory()) {
    Kind = AccessKind::Write;
  } else if (!I.mayReadFromMemory()) {
    report_fatal_error(
        "MemoryAccess::fromInstruction expects a memory access instruction");
  }

  AbstractAccess Access =
      Ptr ? AbstractAccess::fromPointerAndSize(*Ptr, Size, DL)
          : AbstractAccess(nullptr, AbstractValue::unknown(), Size);
  Instruction *Inst = const_cast<Instruction *>(&I);
  return MemoryAccess(ArrayRef<Instruction *>(&Inst, 1), Access, Kind);
}

void MemoryAccess::print(raw_ostream &OS, bool LLVM) const {
  (void)LLVM;
  printAccessKind(OS, Kind);
  if (const auto *Access = getContiguousRegion()) {
    OS << " ptr ";
    Access->print(OS);
    OS << ", ";
    printAbstractValue(OS, Access->getSize());
    return;
  }
  OS << " ";
  getPeriodicRegion()->print(OS);
}

Observation::Observation(StringRef N, const BasicBlock *Block,
                         const Instruction *Anchor, SiteKind K)
    : Observation(N, ObservationSite::anchored(Block, Anchor, K),
                  AlwaysGuard{}) {}

Observation::Observation(StringRef N, ObservationSite S, ObservationGuard G)
    : Name(N.str()), Site(std::move(S)), Guard(std::move(G)) {
  const BasicBlock *Block = Site.getBasicBlock();
  if (Block)
    RepresentedBlocks.insert(Block);
}

Observation::~Observation() { AccessAllocator.DestroyAll(); }

MemoryAccess &Observation::createAccess(ArrayRef<Instruction *> Insts,
                                        const MemoryRegion &Region,
                                        AccessKind Kind) {
  return *new (AccessAllocator.Allocate()) MemoryAccess(Insts, Region, Kind);
}

MemoryAccess &Observation::addAccess(const MemoryAccess &A) {
  auto &Node = createAccess(A.getInstructions(), A.getRegion(), A.getKind());
  Accesses.push_back(Node);
  Node.setParent(this);
  if (MAIRGraph *Graph = getParent())
    Graph->registerAccess(Node);
  ++NumAccesses;
  return Node;
}

std::optional<ObsEdgeKind>
Observation::hasEdgeTo(ArrayRef<ObservationEdge> Edges,
                       const Observation &Obs) {
  const auto *It = llvm::find_if(Edges, [&](const ObservationEdge &Edge) {
    return &Edge.getNode() == &Obs;
  });
  if (It != Edges.end())
    return (*It).getKind();
  return std::nullopt;
}

std::optional<ObsEdgeKind> Observation::hasChild(const Observation &Obs) {
  return Observation::hasEdgeTo(children(), Obs);
}
std::optional<ObsEdgeKind> Observation::hasParent(const Observation &Obs) {
  return Observation::hasEdgeTo(parents(), Obs);
}

bool Observation::hasCompatibleMergeSemantics(const Observation &Other) const {
  if (Guard != Other.Guard)
    return false;
  if (!isEdgeSite() && !Other.isEdgeSite())
    return getSiteKind() == Other.getSiteKind();
  return isEdgeSite() && Other.isEdgeSite() && Site == Other.Site;
}

Observation::iterator Observation::erase(iterator It) {
  assert(It != Accesses.end() && "Cannot erase end iterator");
  if (MAIRGraph *Graph = getParent())
    Graph->unregisterAccess(*It);
  else
    It->clearMemoryObject();
  auto Next = Accesses.erase(It);
  --NumAccesses;
  return Next;
}

void Observation::mergeAccessesFrom(const Observation &Other) {
  assert(hasCompatibleMergeSemantics(Other) &&
         "cannot merge observations with incompatible site or guard semantics");
  for (const MemoryAccess &A : Other.Accesses)
    addAccess(A);
}

void Observation::mergeRepresentationFrom(const Observation &Other) {
  RepresentedBlocks.insert_range(Other.RepresentedBlocks);
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
  OS << Name << ": ";
  if (isEdgeSite() || !hasAlwaysGuard()) {
    OS << "site=";
    Site.print(OS);
    if (const auto *ExecutionGuard =
            std::get_if<AnyProvenanceExecutedGuard>(&Guard)) {
      OS << ", guard=any(";
      bool First = true;
      for (const Instruction *I : ExecutionGuard->getProvenance()) {
        if (!First)
          OS << ", ";
        if (I)
          OS << getPrintedValueName(*I);
        else
          OS << "null";
        First = false;
      }
      OS << ")";
    }
    OS << ", ";
  }
  OS << "preds=[";
  printEdgeList(OS, Parents);
  OS << "], succs=[";
  printEdgeList(OS, Children);
  OS << "]";
  for (const MemoryAccess &A : Accesses) {
    OS << "\n  ";
    A.print(OS, LLVM);
  }
  OS << "\n";
}

MAIRGraph::MAIRGraph()
    : ScalarExprOwner(std::make_unique<MAIRScalarExprOwner>()) {}

void MAIRGraph::assertLive(StringRef Operation) const {
  if (!isLive())
    report_fatal_error(Twine("cannot ") + Operation + " a non-live MAIRGraph");
}

MAIRGraph::MAIRGraph(const Function &Func)
    : F(&Func), ScalarExprOwner(std::make_unique<MAIRScalarExprOwner>()) {}

MAIRGraph::MAIRGraph(Function &Func)
    : F(&Func), MutableF(&Func),
      ScalarExprOwner(std::make_unique<MAIRScalarExprOwner>()) {}

MAIRGraph::MAIRGraph(MAIRGraph &&Other) noexcept
    : F(Other.F), MutableF(Other.MutableF), GraphState(Other.GraphState),
      GraphAllocator(std::move(Other.GraphAllocator)),
      MemoryObjectAllocator(std::move(Other.MemoryObjectAllocator)),
      ScalarExprOwner(std::move(Other.ScalarExprOwner)),
      ScalarExprAllocator(std::move(Other.ScalarExprAllocator)),
      MemoryObjects(std::move(Other.MemoryObjects)),
      BBEntryRepresentatives(std::move(Other.BBEntryRepresentatives)),
      BBExitRepresentatives(std::move(Other.BBExitRepresentatives)),
      Observations(std::move(Other.Observations)),
      NumObservations(Other.NumObservations), NumEdges(Other.NumEdges),
      NextObservationID(Other.NextObservationID) {
  for (Observation &Obs : Observations)
    Obs.setParent(this);
  Other.F = nullptr;
  Other.MutableF = nullptr;
  Other.GraphState = State::MovedFrom;
}

MAIRGraph &MAIRGraph::operator=(MAIRGraph &&Other) noexcept {
  if (this == &Other)
    return *this;
  clearStorage();
  F = Other.F;
  MutableF = Other.MutableF;
  GraphState = Other.GraphState;
  GraphAllocator = std::move(Other.GraphAllocator);
  MemoryObjectAllocator = std::move(Other.MemoryObjectAllocator);
  ScalarExprOwner = std::move(Other.ScalarExprOwner);
  ScalarExprAllocator = std::move(Other.ScalarExprAllocator);
  MemoryObjects = std::move(Other.MemoryObjects);
  BBEntryRepresentatives = std::move(Other.BBEntryRepresentatives);
  BBExitRepresentatives = std::move(Other.BBExitRepresentatives);
  Observations = std::move(Other.Observations);
  NumObservations = Other.NumObservations;
  NumEdges = Other.NumEdges;
  NextObservationID = Other.NextObservationID;
  for (Observation &Obs : Observations)
    Obs.setParent(this);
  Other.F = nullptr;
  Other.MutableF = nullptr;
  Other.GraphState = State::MovedFrom;
  return *this;
}

MAIRGraph::~MAIRGraph() { clearStorage(); }

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

MemoryObject &MAIRGraph::getOrCreateMemoryObject(const Value &Base) {
  MemoryObject *&Object = MemoryObjects[&Base];
  if (!Object)
    Object = new (MemoryObjectAllocator.Allocate()) MemoryObject(Base);
  return *Object;
}

void MAIRGraph::registerAccess(MemoryAccess &Access) {
  const Value *Base = Access.getBase();
  assert(Base && "cannot add a base-less memory access to a MAIRGraph");
  Access.setMemoryObject(&getOrCreateMemoryObject(*Base));
}

void MAIRGraph::unregisterAccess(MemoryAccess &Access) {
  Access.clearMemoryObject();
}

Observation &MAIRGraph::createBBObservation(const BasicBlock &BB,
                                            bool IsEntry) {
  assertLive("create an observation on");
  const Instruction *Anchor =
      IsEntry ? &*BB.getFirstNonPHIIt() : BB.getTerminator();
  Observation &Obs = addObservation(allocateNode<Observation>(
      createObservationName(IsEntry ? "bs" : "be"), &BB, Anchor,
      IsEntry ? Observation::SiteKind::BasicBlockEntry
              : Observation::SiteKind::BasicBlockExit));
  (IsEntry ? BBEntryRepresentatives : BBExitRepresentatives)[&BB] = &Obs;
  return Obs;
}

Observation &MAIRGraph::createInstObservation(const BasicBlock &BB,
                                              const Instruction &Anchor) {
  assertLive("create an observation on");
  auto &Obs = addObservation(
      allocateNode<Observation>(createObservationName("ma"), &BB, &Anchor,
                                Observation::SiteKind::MemoryInst));
  Obs.addAccess(MemoryAccess::fromInstruction(
      Anchor, Anchor.getModule()->getDataLayout()));
  return Obs;
}

Observation &MAIRGraph::createObservation(ObservationSite Site,
                                          ObservationGuard Guard) {
  assertLive("create an observation on");
  StringRef Prefix = Site.isEdge() ? "ve" : "va";
  return addObservation(allocateNode<Observation>(
      createObservationName(Prefix), std::move(Site), std::move(Guard)));
}

Observation &MAIRGraph::createEdgeObservation(const BasicBlock &From,
                                              const BasicBlock &To,
                                              ObservationGuard Guard) {
  assertLive("create an observation on");
  return createObservation(ObservationSite::edge(From, To), std::move(Guard));
}

MAIRScalarExpr MAIRGraph::createConstantExpr(Type &Ty, const APInt &Value) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  auto *Node = new (ScalarExprAllocator.Allocate()) MAIRScalarExprNode(
      *ScalarExprOwner, MAIRScalarExpr::Kind::Constant, Ty, Value, nullptr, {});
  return MAIRScalarExpr(*Node);
}

MAIRScalarExpr MAIRGraph::createValueExpr(const Value &Value) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  auto *Node = new (ScalarExprAllocator.Allocate())
      MAIRScalarExprNode(*ScalarExprOwner, MAIRScalarExpr::Kind::LLVMValue,
                         *Value.getType(), APInt(1, 0), &Value, {});
  return MAIRScalarExpr(*Node);
}

MAIRScalarExpr MAIRGraph::createUnaryExpr(MAIRScalarExpr::Kind Kind,
                                          Type &ResultType,
                                          MAIRScalarExpr Operand) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  MAIRScalarExpr Operands[] = {Operand};
  auto *Node = new (ScalarExprAllocator.Allocate()) MAIRScalarExprNode(
      *ScalarExprOwner, Kind, ResultType, APInt(1, 0), nullptr, Operands);
  return MAIRScalarExpr(*Node);
}

MAIRScalarExpr MAIRGraph::createBinaryExpr(MAIRScalarExpr::Kind Kind,
                                           Type &ResultType, MAIRScalarExpr LHS,
                                           MAIRScalarExpr RHS) {
  assertLive("create an expression on");
  assert(ScalarExprOwner && "cannot create an expression on a moved graph");
  MAIRScalarExpr Operands[] = {LHS, RHS};
  auto *Node = new (ScalarExprAllocator.Allocate()) MAIRScalarExprNode(
      *ScalarExprOwner, Kind, ResultType, APInt(1, 0), nullptr, Operands);
  return MAIRScalarExpr(*Node);
}

bool MAIRGraph::ownsScalarExpr(MAIRScalarExpr Expr) const {
  assertLive("query");
  return ScalarExprOwner && Expr.getOwner() == ScalarExprOwner.get();
}

MAIRGraph::iterator MAIRGraph::erase(Observation &Obs) {
  assertLive("erase from");
  auto It = llvm::find_if(Observations, [&Obs](Observation &Candidate) {
    return &Candidate == &Obs;
  });
  assert(It != Observations.end() &&
         "Observation does not belong to this MAIRGraph");
  SmallVector<const BasicBlock *, 4> EntryKeys;
  SmallVector<const BasicBlock *, 4> ExitKeys;
  for (const auto &Entry : BBEntryRepresentatives)
    if (Entry.second == &Obs)
      EntryKeys.push_back(Entry.first);
  for (const auto &Entry : BBExitRepresentatives)
    if (Entry.second == &Obs)
      ExitKeys.push_back(Entry.first);
  for (const BasicBlock *BB : EntryKeys)
    BBEntryRepresentatives.erase(BB);
  for (const BasicBlock *BB : ExitKeys)
    BBExitRepresentatives.erase(BB);
  SmallVector<Observation *, 4> Succs = successors(Obs);
  SmallVector<Observation *, 4> Preds = predecessors(Obs);
  for (Observation *Succ : Succs)
    removeEdge(Obs, *Succ);
  for (Observation *Pred : Preds)
    removeEdge(*Pred, Obs);
  for (MemoryAccess &Access : Obs.Accesses)
    unregisterAccess(Access);
  auto Next = Observations.erase(It);
  --NumObservations;
  return Next;
}

void MAIRGraph::clearStorage() {
  GraphAllocator.DestroyAll();
  ScalarExprAllocator.DestroyAll();
  MemoryObjectAllocator.DestroyAll();
  Observations.clear();
  MemoryObjects.clear();
  BBEntryRepresentatives.clear();
  BBExitRepresentatives.clear();
  NumObservations = 0;
  NumEdges = 0;
  NextObservationID = 0;
}

void MAIRGraph::clear() {
  assertLive("clear");
  clearStorage();
}

Observation *MAIRGraph::find(StringRef Name) {
  assertLive("find in");
  return const_cast<Observation *>(std::as_const(*this).find(Name));
}

const Observation *MAIRGraph::find(StringRef Name) const {
  assertLive("find in");
  for (const Observation &Obs : Observations)
    if (Obs.getName() == Name)
      return &Obs;
  return nullptr;
}

Observation *MAIRGraph::getBBEntryRepresentative(const BasicBlock &BB) {
  assertLive("query");
  return BBEntryRepresentatives.lookup(&BB);
}

const Observation *
MAIRGraph::getBBEntryRepresentative(const BasicBlock &BB) const {
  assertLive("query");
  return BBEntryRepresentatives.lookup(&BB);
}

Observation *MAIRGraph::getBBExitRepresentative(const BasicBlock &BB) {
  assertLive("query");
  return BBExitRepresentatives.lookup(&BB);
}

const Observation *
MAIRGraph::getBBExitRepresentative(const BasicBlock &BB) const {
  assertLive("query");
  return BBExitRepresentatives.lookup(&BB);
}

void MAIRGraph::replaceObservation(Observation &From, Observation &Into) {
  assertLive("replace observations in");
  assert(From.getParent() == this && Into.getParent() == this &&
         "replacement observations must belong to this graph");
  assert(Into.hasCompatibleMergeSemantics(From) &&
         "replacement observations have incompatible semantics");
  Into.mergeRepresentationFrom(From);
  for (auto &Entry : BBEntryRepresentatives)
    if (Entry.second == &From)
      Entry.second = &Into;
  for (auto &Entry : BBExitRepresentatives)
    if (Entry.second == &From)
      Entry.second = &Into;
}

void MAIRGraph::addEdge(Observation &From, Observation &To, ObsEdgeKind Kind) {
  assertLive("add an edge to");
  assert(From.getParent() == this && To.getParent() == this &&
         "MAIRGraph edge endpoints must belong to this graph");
  if (hasEdge(From, To, Kind))
    return;
  From.Children.push_back(ObservationEdge(To, Kind));
  To.Parents.push_back(ObservationEdge(From, Kind));
  ++NumEdges;
}

void MAIRGraph::removeEdge(Observation &From, Observation &To) {
  assertLive("remove an edge from");
  assert(From.getParent() == this && To.getParent() == this &&
         "MAIRGraph edge endpoints must belong to this graph");
  auto OldChildSize = From.Children.size();
  From.Children.erase(std::remove_if(From.Children.begin(), From.Children.end(),
                                     [&To](const ObservationEdge &Edge) {
                                       return &Edge.getNode() == &To;
                                     }),
                      From.Children.end());
  auto Removed = OldChildSize - From.Children.size();
  if (Removed == 0)
    return;
  To.Parents.erase(std::remove_if(To.Parents.begin(), To.Parents.end(),
                                  [&From](const ObservationEdge &Edge) {
                                    return &Edge.getNode() == &From;
                                  }),
                   To.Parents.end());
  NumEdges -= Removed;
}

bool MAIRGraph::hasEdge(const Observation &From, const Observation &To) const {
  assertLive("query");
  for (const ObservationEdge &Edge : From.children())
    if (&Edge.getNode() == &To)
      return true;
  return false;
}

bool MAIRGraph::hasEdge(const Observation &From, const Observation &To,
                        ObsEdgeKind Kind) const {
  assertLive("query");
  for (const ObservationEdge &Edge : From.children())
    if (&Edge.getNode() == &To && Edge.getKind() == Kind)
      return true;
  return false;
}

MAIRGraph::EdgeListType MAIRGraph::successors(Observation &Obs) {
  assertLive("query");
  EdgeListType Result;
  for (const ObservationEdge &Edge : Obs.children())
    Result.push_back(&Edge.getNode());
  return Result;
}

MAIRGraph::ConstEdgeListType
MAIRGraph::successors(const Observation &Obs) const {
  assertLive("query");
  ConstEdgeListType Result;
  for (const ObservationEdge &Edge : Obs.children())
    Result.push_back(&Edge.getNode());
  return Result;
}

MAIRGraph::ObservationEdgeListType
MAIRGraph::childEdges(const Observation &Obs) const {
  assertLive("query");
  ObservationEdgeListType Result;
  Result.append(Obs.children().begin(), Obs.children().end());
  return Result;
}

MAIRGraph::EdgeListType MAIRGraph::predecessors(Observation &Obs) {
  assertLive("query");
  EdgeListType Result;
  for (const ObservationEdge &Edge : Obs.parents())
    Result.push_back(&Edge.getNode());
  return Result;
}

MAIRGraph::ConstEdgeListType
MAIRGraph::predecessors(const Observation &Obs) const {
  assertLive("query");
  ConstEdgeListType Result;
  for (const ObservationEdge &Edge : Obs.parents())
    Result.push_back(&Edge.getNode());
  return Result;
}

MAIRGraph::ObservationEdgeListType
MAIRGraph::parentEdges(const Observation &Obs) const {
  assertLive("query");
  ObservationEdgeListType Result;
  Result.append(Obs.parents().begin(), Obs.parents().end());
  return Result;
}

MemoryObject *MAIRGraph::getMemoryObject(const Value &Base) {
  assertLive("query");
  return MemoryObjects.lookup(&Base);
}

const MemoryObject *MAIRGraph::getMemoryObject(const Value &Base) const {
  assertLive("query");
  return MemoryObjects.lookup(&Base);
}

std::string MAIRGraph::toString(bool LLVM) const {
  std::string S;
  raw_string_ostream OS(S);
  print(OS, LLVM);
  OS.flush();
  return S;
}

void MAIRGraph::print(raw_ostream &OS, bool LLVM) const {
  assertLive("print");
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
  OS << "}\n\n";
}

} // namespace mair
