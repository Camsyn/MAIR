#pragma once

#include "MAIR/MAIRPasses.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/PassManager.h"

namespace mair {

class MAIRLoopInfo {
  MAIRGraph *Graph = nullptr;
  llvm::FunctionAnalysisManager *FAM = nullptr;
  llvm::Function *Func = nullptr;
  llvm::LoopInfo *LI = nullptr;
  llvm::DominatorTree *DT = nullptr;
  llvm::PostDominatorTree *PDT = nullptr;

public:
  MAIRLoopInfo() = default;
  MAIRLoopInfo(MAIRGraph &G, llvm::FunctionAnalysisManager &FAM);

  bool isAvailable() const { return Graph && FAM && Func && LI && DT && PDT; }

  llvm::Loop *getLoopFor(const Observation &Obs) const;
  bool isSimpleLoop(const llvm::Loop &L) const;
  bool isEveryIterationAccess(const MemoryAccess &Access,
                              const llvm::Loop &L) const;
  bool isGuaranteedOnLoopEntry(const MemoryAccess &Access,
                               const llvm::Loop &L) const;
  bool isLoopInvariant(const MemoryAccess &Access, const llvm::Loop &L) const;
  llvm::Loop *getTopInvariantLoop(const MemoryAccess &Access,
                                  llvm::Loop &Start) const;
  Observation *getPreheaderExit(const llvm::Loop &L) const;
  bool hasDedicatedExit(const llvm::Loop &L) const;
  Observation *getUniqueExitEntry(const llvm::Loop &L) const;
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
