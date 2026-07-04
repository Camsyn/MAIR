#include "MAIR/MAIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/raw_ostream.h"

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

  void Write(const BasicBlock *BB) {
    if (!OS || !BB)
      return;
    *OS << "  basic block: ";
    BB->printAsOperand(*OS, false);
    *OS << "\n";
  }

  void Write(const std::pair<Observation *, Observation *> &Edge) {
    if (!OS)
      return;
    *OS << "  edge: ";
    if (Edge.first)
      *OS << Edge.first->getName();
    else
      *OS << "<null>";
    *OS << " -> ";
    if (Edge.second)
      *OS << Edge.second->getName();
    else
      *OS << "<null>";
    *OS << "\n";
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
};

} // namespace

#define Check(C, ...)                                                          \
  do {                                                                         \
    if (!(C)) {                                                                \
      CheckFailed(__VA_ARGS__);                                                \
    }                                                                          \
  } while (false)

#define CheckFatal(C, ...)                                                     \
  do {                                                                         \
    if (!(C)) {                                                                \
      CheckFailed(__VA_ARGS__);                                                \
      return;                                                                  \
    }                                                                          \
  } while (false)

class MAIRVerifier : public MAIRVerifierSupport {
  const MAIRGraph *Graph = nullptr;

public:
  explicit MAIRVerifier(raw_ostream *OS) : MAIRVerifierSupport(OS) {}

  bool verify(const MAIRGraph &G);

private:
  void verifyObservation(const Observation &Obs);
  void verifyMemoryAccess(const Observation &Obs, const MemoryAccess &Access);
  void verifyEdge(const std::pair<Observation *, Observation *> &Edge);
  void verifyEdgeIndexes();
  void verifyCounters();

  bool hasSuccIndex(const Observation *From, const Observation *To) const;
  bool hasPredIndex(const Observation *To, const Observation *From) const;
  unsigned countSuccIndex(const Observation *From, const Observation *To) const;
  unsigned countPredIndex(const Observation *To, const Observation *From) const;
};

bool MAIRVerifier::verify(const MAIRGraph &G) {
  Graph = &G;
  Broken = false;

  for (const Observation &Obs : G.Observations)
    verifyObservation(Obs);
  for (const auto &Edge : G.Edges)
    verifyEdge(Edge);
  verifyEdgeIndexes();
  verifyCounters();

  return !Broken;
}

void MAIRVerifier::verifyObservation(const Observation &Obs) {
  CheckFatal(Obs.getParent() == Graph,
             "observation parent does not match graph", Obs);

  const BasicBlock *BB = Obs.getBasicBlock();
  CheckFatal(BB, "observation has no basic block", Obs);

  const Instruction *Anchor = Obs.getAnchor();
  CheckFatal(Anchor, "observation has no anchor", Obs);
  CheckFatal(Anchor->getParent() == BB,
             "observation anchor is outside its block", Obs, Anchor, BB);

  if (Obs.isBasicBlockEntrySite()) {
    Check(Anchor == &*BB->getFirstNonPHIIt(),
          "basic-block entry observation is not anchored at first non-PHI", Obs,
          Anchor, BB);
  }

  if (Obs.isBasicBlockExitSite()) {
    Check(Anchor == BB->getTerminator(),
          "basic-block exit observation is not anchored at terminator", Obs,
          Anchor, BB);
  }

  for (const MemoryAccess &Access : Obs)
    verifyMemoryAccess(Obs, Access);
}

void MAIRVerifier::verifyMemoryAccess(const Observation &Obs,
                                      const MemoryAccess &Access) {
  Check(Access.getParent() == &Obs,
        "memory access parent does not match containing observation", Access,
        Obs);

  if (Obs.isMemoryInstSite()) {
    Check(Access.getInstruction() == Obs.getAnchor(),
          "memory access instruction does not match observation anchor", Access,
          Obs, Access.getInstruction(), Obs.getAnchor());
  }
}

void MAIRVerifier::verifyEdge(
    const std::pair<Observation *, Observation *> &Edge) {
  CheckFatal(Edge.first, "edge has null source", Edge);
  CheckFatal(Edge.second, "edge has null destination", Edge);
  CheckFatal(Edge.first->getParent() == Graph,
             "edge source observation does not belong to graph", Edge,
             Edge.first);
  CheckFatal(Edge.second->getParent() == Graph,
             "edge destination observation does not belong to graph", Edge,
             Edge.second);
  Check(hasSuccIndex(Edge.first, Edge.second),
        "edge is missing from successor index", Edge, Edge.first, Edge.second);
  Check(hasPredIndex(Edge.second, Edge.first),
        "edge is missing from predecessor index", Edge, Edge.first,
        Edge.second);
  Check(llvm::count(Graph->Edges, Edge) == 1,
        "edge appears more than once in edge list", Edge, Edge.first,
        Edge.second);
  Check(countSuccIndex(Edge.first, Edge.second) == 1,
        "edge appears more than once in successor index", Edge, Edge.first,
        Edge.second);
  Check(countPredIndex(Edge.second, Edge.first) == 1,
        "edge appears more than once in predecessor index", Edge, Edge.first,
        Edge.second);
}

void MAIRVerifier::verifyEdgeIndexes() {
  for (const auto &Entry : Graph->SuccMap) {
    const Observation *From = Entry.first;
    CheckFatal(From, "successor index has null source observation");
    CheckFatal(From->getParent() == Graph,
               "successor index source observation does not belong to graph",
               From);
    Check(!Entry.second.empty(), "successor index has empty edge list", From);
    for (const Observation *To : Entry.second) {
      CheckFatal(To, "successor index has null destination observation", From);
      Check(llvm::is_contained(Graph->Edges,
                               std::make_pair(const_cast<Observation *>(From),
                                              const_cast<Observation *>(To))),
            "successor index contains edge missing from edge list", From, To);
    }
  }

  for (const auto &Entry : Graph->PredMap) {
    const Observation *To = Entry.first;
    CheckFatal(To, "predecessor index has null destination observation");
    CheckFatal(To->getParent() == Graph,
               "predecessor index destination observation does not belong to "
               "graph",
               To);
    Check(!Entry.second.empty(), "predecessor index has empty edge list", To);
    for (const Observation *From : Entry.second) {
      CheckFatal(From, "predecessor index has null source observation", To);
      Check(llvm::is_contained(Graph->Edges,
                               std::make_pair(const_cast<Observation *>(From),
                                              const_cast<Observation *>(To))),
            "predecessor index contains edge missing from edge list", From, To);
    }
  }
}

void MAIRVerifier::verifyCounters() {
  unsigned ObservedNodes = 0;
  for (const Observation &Obs : Graph->Observations) {
    ++ObservedNodes;

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
  Check(Graph->Edges.size() == Graph->NumEdges,
        "graph edge count does not match edge list");
}

bool MAIRVerifier::hasSuccIndex(const Observation *From,
                                const Observation *To) const {
  auto It = Graph->SuccMap.find(const_cast<Observation *>(From));
  return It != Graph->SuccMap.end() &&
         llvm::is_contained(It->second, const_cast<Observation *>(To));
}

bool MAIRVerifier::hasPredIndex(const Observation *To,
                                const Observation *From) const {
  auto It = Graph->PredMap.find(const_cast<Observation *>(To));
  return It != Graph->PredMap.end() &&
         llvm::is_contained(It->second, const_cast<Observation *>(From));
}

unsigned MAIRVerifier::countSuccIndex(const Observation *From,
                                      const Observation *To) const {
  auto It = Graph->SuccMap.find(const_cast<Observation *>(From));
  if (It == Graph->SuccMap.end())
    return 0;
  return llvm::count(It->second, const_cast<Observation *>(To));
}

unsigned MAIRVerifier::countPredIndex(const Observation *To,
                                      const Observation *From) const {
  auto It = Graph->PredMap.find(const_cast<Observation *>(To));
  if (It == Graph->PredMap.end())
    return 0;
  return llvm::count(It->second, const_cast<Observation *>(From));
}

bool MAIRGraph::verify(raw_ostream *OS) const {
  // Keep the no-diagnostics path cheap; printing MAIR objects is relatively
  // expensive and should only happen on explicit request.
  MAIRVerifier Verifier(OS);
  return Verifier.verify(*this);
}

} // namespace mair

#undef Check
#undef CheckFatal
