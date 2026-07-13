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
  void verifyChildEdge(const Observation &From, const ObservationEdge &Edge);
  void verifyParentEdge(const Observation &To, const ObservationEdge &Edge);
  void verifyCounters();
  void verifyBoundaryRepresentatives();

  unsigned countChildEdge(const Observation &From, const Observation &To,
                          ObsEdgeKind Kind) const;
  unsigned countParentEdge(const Observation &To, const Observation &From,
                           ObsEdgeKind Kind) const;
};

bool MAIRVerifier::verify(const MAIRGraph &G) {
  Graph = &G;
  Broken = false;

  for (const Observation &Obs : G.Observations)
    verifyObservation(Obs);
  verifyBoundaryRepresentatives();
  verifyCounters();

  return !Broken;
}

void MAIRVerifier::verifyBoundaryRepresentatives() {
  auto IsLive = [&](const Observation *Representative) {
    return llvm::any_of(Graph->Observations, [&](const Observation &Obs) {
      return &Obs == Representative;
    });
  };
  for (const auto &Entry : Graph->BBEntryRepresentatives) {
    const BasicBlock *BB = Entry.first;
    const Observation *Representative = Entry.second;
    CheckFatal(BB && Representative, "invalid BB-entry representative");
    CheckFatal(IsLive(Representative), "BB-entry representative is not live",
               Representative, BB);
    Check(Representative->getParent() == Graph,
          "BB-entry representative does not belong to graph", Representative,
          BB);
    Check(Representative->representsBlock(*BB),
          "BB-entry representative does not represent its block",
          Representative, BB);
  }
  for (const auto &Entry : Graph->BBExitRepresentatives) {
    const BasicBlock *BB = Entry.first;
    const Observation *Representative = Entry.second;
    CheckFatal(BB && Representative, "invalid BB-exit representative");
    CheckFatal(IsLive(Representative), "BB-exit representative is not live",
               Representative, BB);
    Check(Representative->getParent() == Graph,
          "BB-exit representative does not belong to graph", Representative,
          BB);
    Check(Representative->representsBlock(*BB),
          "BB-exit representative does not represent its block", Representative,
          BB);
  }
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

  for (const ObservationEdge &Edge : Obs.children())
    verifyChildEdge(Obs, Edge);
  for (const ObservationEdge &Edge : Obs.parents())
    verifyParentEdge(Obs, Edge);
}

void MAIRVerifier::verifyMemoryAccess(const Observation &Obs,
                                      const MemoryAccess &Access) {
  Check(Access.getParent() == &Obs,
        "memory access parent does not match containing observation", Access,
        Obs);

  if (Obs.isMemoryInstSite()) {
    const Instruction *AccessI = Access.getInstruction();
    CheckFatal(AccessI, "memory observation has access without instruction",
               Access, Obs);
    Check(AccessI->getFunction() == Obs.getAnchor()->getFunction(),
          "memory access instruction is outside observation function", Access,
          Obs, AccessI, Obs.getAnchor());
  }

  const Value *Base = Access.getBase();
  CheckFatal(Base, "memory access has no base", Access, Obs);
  CheckFatal(Access.getBaseUse().get(),
             "memory access is not linked to a memory object", Access, Obs,
             Base);
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
}

void MAIRVerifier::verifyChildEdge(const Observation &From,
                                   const ObservationEdge &Edge) {
  const Observation &To = Edge.getNode();
  CheckFatal(To.getParent() == Graph,
             "child edge destination observation does not belong to graph",
             From);
  Check(countChildEdge(From, To, Edge.getKind()) == 1,
        "child edge appears more than once", From);
  Check(countParentEdge(To, From, Edge.getKind()) == 1,
        "child edge is missing from destination parent list", From);
}

void MAIRVerifier::verifyParentEdge(const Observation &To,
                                    const ObservationEdge &Edge) {
  const Observation &From = Edge.getNode();
  CheckFatal(From.getParent() == Graph,
             "parent edge source observation does not belong to graph", To);
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
  return llvm::count_if(From.children(), [&To, Kind](const auto &Edge) {
    return &Edge.getNode() == &To && Edge.getKind() == Kind;
  });
}

unsigned MAIRVerifier::countParentEdge(const Observation &To,
                                       const Observation &From,
                                       ObsEdgeKind Kind) const {
  return llvm::count_if(To.parents(), [&From, Kind](const auto &Edge) {
    return &Edge.getNode() == &From && Edge.getKind() == Kind;
  });
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
