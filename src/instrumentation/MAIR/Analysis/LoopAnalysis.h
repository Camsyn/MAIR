#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/PassManager.h"

namespace mair {

struct MAIRLoopExit {
  llvm::BasicBlock *Exiting = nullptr;
  llvm::BasicBlock *Exit = nullptr;
  bool IsDedicated = false;
};

class MAIRLoopInfo {
  MAIRGraph *Graph = nullptr;
  llvm::FunctionAnalysisManager *FAM = nullptr;
  llvm::Function *Func = nullptr;
  const llvm::LoopInfo *LI = nullptr;
  const llvm::DominatorTree *DT = nullptr;
  const llvm::PostDominatorTree *PDT = nullptr;

public:
  MAIRLoopInfo() = default;
  MAIRLoopInfo(MAIRGraph &G, llvm::FunctionAnalysisManager &FAM);

  bool isAvailable() const { return Graph && FAM && Func && LI && DT && PDT; }

  llvm::Loop *getLoopFor(const ProgramPoint &PP) const;
  bool isValueAvailableAt(const llvm::Value &V,
                          const ProgramPointSite &Site) const;
  bool isValueAvailableAt(const AbstractValue &V,
                          const ProgramPointSite &Site) const;
  bool executesOncePerRelevantIteration(const ProgramPoint &PP,
                                        const llvm::Loop &L) const;
  const llvm::SCEV *getExactExecutionCountSCEV(const ProgramPoint &PP,
                                               const llvm::Loop &L) const;
  bool isLoopInvariant(const Observation &Obs, const llvm::Loop &L) const;
  llvm::Loop *getTopInvariantLoop(const Observation &Obs,
                                  llvm::Loop &Start) const;
  bool canHoistToPreheader(const ProgramPoint &PP, const llvm::Loop &L) const;
  bool canSinkToExit(const ProgramPoint &PP, const llvm::Loop &L) const;
  ProgramPoint *getPreheaderExit(const llvm::Loop &L) const;
  std::optional<MAIRLoopExit> describeUniqueExit(const llvm::Loop &L) const;
  ProgramPoint *getUniqueExitEntry(const llvm::Loop &L) const;
  /// Where an observation that leaves \p L through its unique exit is placed:
  /// the exit block entry if only \p L reaches that block, otherwise the exit
  /// edge. Empty if \p L has no unique exit.
  std::optional<ProgramPointSite> getExitSite(const llvm::Loop &L) const;
  /// The program point of the parent graph at \p Site, from getExitSite, with
  /// \p Guard: the exit boundary of \p L if it fits, otherwise the first match
  /// in list order. Null if there is none yet.
  ProgramPoint *findExitPoint(const llvm::Loop &L, const ProgramPointSite &Site,
                              const ProgramPointGuard &Guard) const;
  /// ScalarEvolution must come from the same FAM generation as the graph's
  /// borrowed LoopInfo; mixing snapshots would invalidate SCEV loop identity.
  llvm::ScalarEvolution &getSE() const;

private:
  bool isLoopInvariant(const llvm::Value *V, const llvm::Loop &L) const;
  bool isLoopInvariant(const AbstractValue &Value, const llvm::Loop &L) const;
};

class LoopAnalysis : public llvm::AnalysisInfoMixin<LoopAnalysis> {
  friend llvm::AnalysisInfoMixin<LoopAnalysis>;
  static llvm::AnalysisKey Key;

  llvm::FunctionAnalysisManager *FAM = nullptr;

public:
  using Result = MAIRLoopInfo;

  explicit LoopAnalysis(llvm::FunctionAnalysisManager *FAM = nullptr)
      : FAM(FAM) {}

  Result run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
