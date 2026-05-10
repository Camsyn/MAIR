//===----------------------------------------------------------------------===//
//
// UBSan Inst Tagging
//
// Problem
// -------
// UBSan frontend IR is not always fully marked, so XSan classifies a use-def
// closure instead of individual `!nosanitize` instructions: values used only by
// UBSan are UBSan IR; values that can reach ordinary user exits stay user IR.
//
// Graph
// -----
// The closure is ordinary dominance over a synthetic instruction graph:
//
//   VEntry -> UBSanVRoot -> UBSan roots -> operand instructions
//          \ UserVRoot  -> User roots  -> operand instructions
//
// Instruction edges point to instruction operands.  User roots are terminators
// and values without users; they keep shared operands out of the UBSan tree.
// Descendants of `UBSanVRoot` are the UBSan closure.
//
// Flow
// ----
// 1. Collect UBSan handler-call roots and user-exit roots, then build the first
//    `UBSanUseDefDomTree`.
// 2. Tag the initial UBSan closure so UBSan-only handler blocks are visible.
// 3. Promote handler-block control roots: the handler block must end in an
//    unconditional branch, and conditional predecessor branches need `!prof`.
// 4. Batch-update the root edges and dominator tree, then tag final
//    `UBSanVRoot` descendants as UBSan and drop stale `UserIR`.
//
// LLVM15 note: `GenericDomTreeConstruction.h` is included directly because the
// custom `DomTreeBase<InstDefUseNode>` instantiation needs the template bodies
// for full calculation and updates.
//
//===----------------------------------------------------------------------===//

#include "UbsanInstTagging.hpp"
#include "Utils/MetaDataUtils.h"
#include "Utils/UbsanUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/iterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Use.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/GenericDomTreeConstruction.h" // IWYU pragma: keep
#include <array>
#include <type_traits>
#include <utility>
#include <variant>

using namespace llvm;

UbsanInstTaggingPass::UbsanInstTaggingPass() {}

namespace {

bool isUserRootInstruction(const Instruction &I);
bool isIgnorableForUbsanBlock(const Instruction &I);

struct InstUseDefGraph;
class UBSanUseDefDomTree;

struct InstDefUseNode;
class InstDefUseChildIterator;

struct InstDefUseNode {
  friend struct InstUseDefGraph;

  enum class Tag {
    VEntry,
    UBSanVRoot,
    UBSanRoot,
    UserVRoot,
    UserRoot,
    Instruction
  };

  Instruction *Inst = nullptr;
  InstUseDefGraph &Graph;
  Tag NodeTag = Tag::Instruction;

  bool isInstructionNode() const { return Inst != nullptr; }
  bool canTrackDefUse() const { return NodeTag == Tag::Instruction; }
  bool canTrackUseDef() const { return isInstructionNode(); }

  bool isUBSanRoot() const { return NodeTag == Tag::UBSanRoot; }

  void printAsOperand(raw_ostream &OS, bool) const {
    if (NodeTag == Tag::Instruction) {
      Inst->printAsOperand(OS, false);
      return;
    }
    switch (NodeTag) {
    case Tag::VEntry:
      OS << "<<ventry>>";
      break;
    case Tag::UBSanVRoot:
      OS << "<<ubsan-vroot>>";
      break;
    case Tag::UserVRoot:
      OS << "<<user-vroot>>";
      break;
    case Tag::UBSanRoot:
    case Tag::UserRoot:
    case Tag::Instruction:
      OS << "<<instruction>>";
      break;
    }
  }

  InstDefUseNode(InstUseDefGraph &Graph, Tag NodeTag)
      : Graph(Graph), NodeTag(NodeTag) {}
  InstDefUseNode(InstUseDefGraph &Graph, Instruction &I)
      : InstDefUseNode(Graph, I, Tag::Instruction) {}
  InstDefUseNode(InstUseDefGraph &Graph, Instruction &I, Tag NodeTag)
      : Graph(Graph), NodeTag(NodeTag), Inst(&I) {}
  InstUseDefGraph *getParent() const { return &Graph; }
};

/// Graph topology:
///           UBSanVRoot -- [UBSanRoots, ...]  -- [Instructions, ...]
///          /
/// VEntry --
///          \
///           UserVRoot  -- [UserRoots, ...]  -- [Instructions, ...]
struct InstUseDefGraph {
  using Tag = InstDefUseNode::Tag;

private:
  class NodeStorage {
    SmallVector<InstDefUseNode, 32> Storage;
    DenseMap<Instruction *, InstDefUseNode *> InstToNode;
    InstUseDefGraph &Graph;

  public:
    NodeStorage(InstUseDefGraph &Graph, unsigned ReservedSize) : Graph(Graph) {
      Storage.reserve(ReservedSize + 3);
      InstToNode.reserve(ReservedSize);
    }
    InstDefUseNode &createNode(Instruction &I) {
      Storage.emplace_back(Graph, I);
      auto &Node = Storage.back();
      InstToNode[&I] = &Node;
      return Node;
    }
    InstDefUseNode &createNode(Tag Tag) {
      Storage.emplace_back(Graph, Tag);
      return Storage.back();
    }

    InstDefUseNode *getNode(const Instruction &I) const {
      const auto It = InstToNode.find(&I);
      assert(It != InstToNode.end() && "instruction node should already exist");
      return It->second;
    }

    auto begin() { return Storage.begin(); }
    auto end() { return Storage.end(); }
    unsigned size() const { return Storage.size(); }
  } Storage;

public:
  InstDefUseNode *VEntry;
  InstDefUseNode *UBSanVRoot;
  InstDefUseNode *UserVRoot;
  SmallVector<InstDefUseNode *> UBSanRoots;
  SmallVector<InstDefUseNode *> UserRoots;

private:
  Function &F;
  std::array<InstDefUseNode *, 2> VRoots;

public:
  template <typename UBSanRootRangeT, typename UserRootRangeT>
  InstUseDefGraph(Function &F, const UBSanRootRangeT &UBSanRoots,
                  const UserRootRangeT &UserRoots)
      : F(F), Storage(*this, F.getInstructionCount()),
        VEntry(&Storage.createNode(Tag::VEntry)),
        UBSanVRoot(&Storage.createNode(Tag::UBSanVRoot)),
        UserVRoot(&Storage.createNode(Tag::UserVRoot)),
        VRoots({UBSanVRoot, UserVRoot}) {
    for (Instruction &I : instructions(F))
      Storage.createNode(I);

    addUBSanRoots(UBSanRoots);
    addUserRoots(UserRoots);
  }

  InstDefUseNode *getNode(const Instruction &I) const {
    return Storage.getNode(I);
  }

  template <typename RootsT> void addUBSanRoots(const RootsT &Roots) {
    UBSanRoots.reserve(UBSanRoots.size() + Roots.size());
    for (Instruction *I : Roots) {
      InstDefUseNode *const Node = getNode(*I);
      Node->NodeTag = InstDefUseNode::Tag::UBSanRoot;
      UBSanRoots.push_back(Node);
    }
  }

  template <typename RootsT>
  void addUBSanRoots(const RootsT &Roots,
                     SmallVectorImpl<InstDefUseNode *> &AddedNodes) {
    UBSanRoots.reserve(UBSanRoots.size() + Roots.size());
    for (Instruction *I : Roots) {
      InstDefUseNode *const Node = getNode(*I);
      if (llvm::is_contained(UBSanRoots, Node))
        continue;
      Node->NodeTag = InstDefUseNode::Tag::UBSanRoot;
      UBSanRoots.push_back(Node);
      AddedNodes.push_back(Node);
    }
  }

  template <typename RootsT> void addUserRoots(const RootsT &Roots) {
    UserRoots.reserve(UserRoots.size() + Roots.size());
    for (Instruction *I : Roots) {
      InstDefUseNode *const Node = getNode(*I);
      Node->NodeTag = InstDefUseNode::Tag::UserRoot;
      UserRoots.push_back(Node);
    }
  }

  template <typename RootsT>
  void removeUserRoots(const RootsT &Roots,
                       SmallVectorImpl<InstDefUseNode *> &RemovedNodes) {
    unsigned WriteIdx = 0;
    bool SawRemoval = false;
    for (InstDefUseNode *Node : UserRoots) {
      if (llvm::is_contained(Roots, Node->Inst)) {
        RemovedNodes.push_back(Node);
        SawRemoval = true;
        continue;
      }
      if (SawRemoval)
        UserRoots[WriteIdx] = Node;
      ++WriteIdx;
    }
    if (SawRemoval)
      UserRoots.resize(WriteIdx);
  }

  bool isUBSanRoot(const Instruction &I) const {
    return getNode(I)->isUBSanRoot();
  }

  auto children(const Instruction &I) const {
    auto mapped = map_range(I.operand_values(), [this](const Value *V) {
      const auto *OpI = dyn_cast<Instruction>(V);
      if (!OpI)
        return static_cast<InstDefUseNode *>(nullptr);
      return this->getNode(*OpI);
    });
    return mapped;
  }
  ArrayRef<InstDefUseNode *> children(Tag tag) const {
    assert(tag != Tag::Instruction);
    switch (tag) {
    case Tag::VEntry:
      return VRoots;
    case Tag::UBSanVRoot:
      return UBSanRoots;
    case Tag::UserVRoot:
      return UserRoots;
    default:
      assert(false && "Unexpected type");
    }
  }
  auto parents(const Instruction &I) const {
    auto mapped = map_range(I.users(), [this](const User *V) {
      const auto *UserI = dyn_cast<Instruction>(V);
      if (!UserI)
        return static_cast<InstDefUseNode *>(nullptr);
      return this->getNode(*UserI);
    });
    return mapped;
  }

  ArrayRef<InstDefUseNode *> parents(Tag tag) const {
    assert(tag != Tag::Instruction);
    switch (tag) {
    case Tag::VEntry:
      return {};
    case Tag::UBSanVRoot:
    case Tag::UserVRoot:
      return ArrayRef<InstDefUseNode *>(&VEntry, 1);
    case Tag::UBSanRoot:
      return ArrayRef<InstDefUseNode *>(&UBSanVRoot, 1);
    case Tag::UserRoot:
      return ArrayRef<InstDefUseNode *>(&UserVRoot, 1);
    default:
      assert(false && "Unexpected type");
    }
  }

  InstDefUseNode &front() { return *VEntry; }
  const InstDefUseNode &front() const { return *VEntry; }
  auto nodes_begin() { return Storage.begin(); }
  auto nodes_end() { return Storage.end(); }
  unsigned size() const { return Storage.size(); }
};

// A interator to unifiy different types of iterators.
class InstDefUseChildIterator {
  using ArrayIter = ArrayRef<InstDefUseNode *>::iterator;
  using ChildRange = decltype(std::declval<const InstUseDefGraph &>().children(
      std::declval<const Instruction &>()));
  using ChildIter = decltype(std::declval<ChildRange>().begin());
  using ParentRange = decltype(std::declval<const InstUseDefGraph &>().parents(
      std::declval<const Instruction &>()));
  using ParentIter = decltype(std::declval<ParentRange>().begin());
  using IteratorState =
      std::variant<std::monostate, ArrayIter, ChildIter, ParentIter>;

  InstDefUseNode *Owner = nullptr;
  bool Inverse = false;
  IteratorState State;

public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = InstDefUseNode *;
  using difference_type = std::ptrdiff_t;
  using pointer = InstDefUseNode **;
  using reference = InstDefUseNode *;

  InstDefUseChildIterator() = default;
  InstDefUseChildIterator(const InstUseDefGraph &Graph, InstDefUseNode *Node,
                          bool Inverse, bool End)
      : Owner(Node), Inverse(Inverse) {
    if (!Inverse && Node->canTrackUseDef()) {
      auto Range = Graph.children(*Node->Inst);
      State.emplace<ChildIter>(End ? Range.end() : Range.begin());
      return;
    }
    if (Inverse && Node->canTrackDefUse()) {
      auto Range = Graph.parents(*Node->Inst);
      State.emplace<ParentIter>(End ? Range.end() : Range.begin());
      return;
    }

    auto Range =
        Inverse ? Graph.parents(Node->NodeTag) : Graph.children(Node->NodeTag);
    State.emplace<ArrayIter>(End ? Range.end() : Range.begin());
  }

  bool operator==(const InstDefUseChildIterator &Other) const {
    return Owner == Other.Owner && Inverse == Other.Inverse &&
           State == Other.State;
  }

  bool operator!=(const InstDefUseChildIterator &Other) const {
    return !(*this == Other);
  }

  InstDefUseNode *operator*() const {
    return std::visit(
        [](const auto &It) -> InstDefUseNode * {
          using IteratorT = std::decay_t<decltype(It)>;
          if constexpr (std::is_same_v<IteratorT, std::monostate>)
            llvm_unreachable("cannot dereference empty def-use iterator");
          else
            return *It;
        },
        State);
  }

  InstDefUseChildIterator &operator++() {
    std::visit(
        [](auto &It) {
          using IteratorT = std::decay_t<decltype(It)>;
          if constexpr (std::is_same_v<IteratorT, std::monostate>)
            llvm_unreachable("cannot increment empty def-use iterator");
          else
            ++It;
        },
        State);
    return *this;
  }

  InstDefUseChildIterator operator++(int) {
    InstDefUseChildIterator Copy = *this;
    ++*this;
    return Copy;
  }

  InstDefUseChildIterator &operator--() {
    std::visit(
        [](auto &It) {
          using IteratorT = std::decay_t<decltype(It)>;
          if constexpr (std::is_same_v<IteratorT, std::monostate>)
            llvm_unreachable("cannot decrement empty def-use iterator");
          else if constexpr (!std::is_base_of_v<
                                 std::bidirectional_iterator_tag,
                                 typename std::iterator_traits<
                                     IteratorT>::iterator_category>)
            llvm_unreachable("cannot decrement forward-only def-use iterator");
          else
            --It;
        },
        State);
    return *this;
  }

  InstDefUseChildIterator operator--(int) {
    InstDefUseChildIterator Copy = *this;
    --*this;
    return Copy;
  }
};

} // namespace

namespace llvm {

template <> struct GraphTraits<InstDefUseNode *> {
  using NodeRef = InstDefUseNode *;
  using ChildIteratorType = InstDefUseChildIterator;
  using nodes_iterator =
      pointer_iterator<SmallVector<InstDefUseNode, 32>::iterator>;

  static NodeRef getEntryNode(NodeRef Node) { return Node; }
  static ChildIteratorType child_begin(NodeRef Node) {
    return ChildIteratorType(Node->Graph, Node, /*Inverse=*/false,
                             /*End=*/false);
  }
  static ChildIteratorType child_end(NodeRef Node) {
    return ChildIteratorType(Node->Graph, Node, /*Inverse=*/false,
                             /*End=*/true);
  }
  static nodes_iterator nodes_begin(NodeRef Node) {
    return nodes_iterator(Node->Graph.nodes_begin());
  }
  static nodes_iterator nodes_end(NodeRef Node) {
    return nodes_iterator(Node->Graph.nodes_end());
  }
  static unsigned size(NodeRef Node) { return Node->Graph.size(); }
};

template <> struct GraphTraits<Inverse<InstDefUseNode *>> {
  using NodeRef = InstDefUseNode *;
  using ChildIteratorType = InstDefUseChildIterator;

  static NodeRef getEntryNode(Inverse<NodeRef> G) { return G.Graph; }
  static ChildIteratorType child_begin(NodeRef Node) {
    return ChildIteratorType(Node->Graph, Node, /*Inverse=*/true,
                             /*End=*/false);
  }
  static ChildIteratorType child_end(NodeRef Node) {
    return ChildIteratorType(Node->Graph, Node, /*Inverse=*/true,
                             /*End=*/true);
  }
};

template <>
struct GraphTraits<InstUseDefGraph *> : GraphTraits<InstDefUseNode *> {
  static NodeRef getEntryNode(InstUseDefGraph *Graph) { return Graph->VEntry; }
  static nodes_iterator nodes_begin(InstUseDefGraph *Graph) {
    return nodes_iterator(Graph->nodes_begin());
  }
  static nodes_iterator nodes_end(InstUseDefGraph *Graph) {
    return nodes_iterator(Graph->nodes_end());
  }
  static unsigned size(InstUseDefGraph *Graph) { return Graph->size(); }
};

} // namespace llvm

namespace {

class UBSanUseDefDomTree : public DomTreeBase<InstDefUseNode> {
  const InstUseDefGraph &getGraph() const {
    assert(this->Parent && "UBSanUseDomTree must be calculated first");
    return *this->Parent;
  }

public:
  using Base = DomTreeBase<InstDefUseNode>;
  using Base::recalculate;

  SmallVector<Instruction *, 32> getAllUBSanInsts() const;
  bool tagAllUBSanInsts();
};

bool isUBSanBlock(const BasicBlock &BB) {
  const auto *Term = dyn_cast<BranchInst>(BB.getTerminator());
  if (!Term || Term->isConditional())
    return false;

  return all_of(BB, [](const Instruction &I) {
    return I.isTerminator() || isIgnorableForUbsanBlock(I) ||
           __xsan::UBSanInst::is(I);
  });
}

void getNewRootsFromUBSanBB(ArrayRef<Instruction *> UbsanCallAnchors,
                            SmallSetVector<Instruction *, 8> &NewRoots) {
  SmallVector<BasicBlock *, 8> UbsanBlocks;
  for (Instruction *Anchor : UbsanCallAnchors) {
    BasicBlock *BB = Anchor->getParent();
    if (isUBSanBlock(*BB) && !llvm::is_contained(UbsanBlocks, BB))
      UbsanBlocks.push_back(BB);
  }

  for (BasicBlock *BB : UbsanBlocks) {
    NewRoots.insert(BB->getTerminator());

    for (BasicBlock *Pred : predecessors(BB)) {
      auto *Br = dyn_cast<BranchInst>(Pred->getTerminator());
      // UBSan handle call must be guarded by a UNLIKELY predicate.
      if (!Br || !Br->isConditional() || !Br->getMetadata(LLVMContext::MD_prof))
        continue;
      NewRoots.insert(Br);
    }
  }
}

SmallVector<Instruction *, 32> UBSanUseDefDomTree::getAllUBSanInsts() const {
  const InstUseDefGraph &Graph = getGraph();
  SmallVector<Instruction *, 32> UbsanInsts;
  SmallVector<InstDefUseNode *, 32> DTNodes;
  this->getDescendants(Graph.UBSanVRoot, DTNodes);
  for (InstDefUseNode *Node : DTNodes) {
    if (!Node || !Node->isInstructionNode())
      continue;
    UbsanInsts.push_back(Node->Inst);
  }

  return UbsanInsts;
}

bool UBSanUseDefDomTree::tagAllUBSanInsts() {
  bool Changed = false;
  for (Instruction *I : getAllUBSanInsts()) {
    __xsan::UBSanInst::set(*I);
    __xsan::UserIR::drop(*I);
    Changed = true;
  }
  return Changed;
}

// Those instructions without user
bool isUserRootInstruction(const Instruction &I) {
  return I.user_empty() || I.isTerminator();
}

bool isIgnorableForUbsanBlock(const Instruction &I) {
  return isa<DbgInfoIntrinsic>(I);
}

bool tagUbsanInstructions(Function &F) {
  if (F.isDeclaration())
    return false;

  //===------------------------------------------------------------------===//
  // Phase 1: seed explicit UBSan handler calls.
  //===------------------------------------------------------------------===//
  SmallVector<Instruction *, 8> UBSanHandlerRoots, UserRoots;
  for (Instruction &I : instructions(F)) {
    if (__xsan::isUbsanInterfaceCall(I))
      UBSanHandlerRoots.push_back(&I);
    else if (isUserRootInstruction(I))
      UserRoots.push_back(&I);
  }

  //===------------------------------------------------------------------===//
  // Phase 2: build the initial instruction use-def dominator tree.
  //===------------------------------------------------------------------===//
  InstUseDefGraph Graph(F, UBSanHandlerRoots, UserRoots);
  UBSanUseDefDomTree DT;
  DT.recalculate(Graph);
  bool Changed = DT.tagAllUBSanInsts();

  //===------------------------------------------------------------------===//
  // Phase 3: use handler calls as anchors to locate UBSan-only control blocks
  // and collect new roots (i.e., conditional terminator -> UBSan BB).
  //===------------------------------------------------------------------===//
  SmallSetVector<Instruction *, 8> NewRoots;
  getNewRootsFromUBSanBB(UBSanHandlerRoots, NewRoots);

  //===------------------------------------------------------------------===//
  // Phase 4: update the tree with newly discovered control roots.
  //===------------------------------------------------------------------===//
  SmallVector<InstDefUseNode *, 8> AddedUBSanRoots;
  SmallVector<InstDefUseNode *, 8> RemovedUserRoots;
  Graph.addUBSanRoots(NewRoots, AddedUBSanRoots);
  Graph.removeUserRoots(NewRoots, RemovedUserRoots);

  SmallVector<UBSanUseDefDomTree::UpdateType, 8> Updates;
  for (InstDefUseNode *Node : AddedUBSanRoots)
    Updates.push_back({UBSanUseDefDomTree::Insert, Graph.UBSanVRoot, Node});
  for (InstDefUseNode *Node : RemovedUserRoots)
    Updates.push_back({UBSanUseDefDomTree::Delete, Graph.UserVRoot, Node});

  if (!AddedUBSanRoots.empty() || !RemovedUserRoots.empty())
    Changed = true;
  if (!Updates.empty())
    DT.applyUpdates(Updates);

  //===------------------------------------------------------------------===//
  // Phase 5: tag the final UBSan closure from the finalized tree.
  //===------------------------------------------------------------------===//
  Changed |= DT.tagAllUBSanInsts();

  return Changed;
}

} // namespace

PreservedAnalyses UbsanInstTaggingPass::run(Module &M,
                                            ModuleAnalysisManager &_) {
  if (!__xsan::hasUbsanFunction(M))
    return PreservedAnalyses::all();

  for (Function &F : M)
    tagUbsanInstructions(F);

  return PreservedAnalyses::all();
}
