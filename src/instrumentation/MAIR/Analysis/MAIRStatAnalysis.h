#pragma once

#include "MAIR/MAIRPasses.h"

namespace mair {

class MAIRStat {
  unsigned NumObservations = 0;
  unsigned NumMemoryAccesses = 0;

public:
  MAIRStat() = default;
  MAIRStat(unsigned Observations, unsigned MemoryAccesses)
      : NumObservations(Observations), NumMemoryAccesses(MemoryAccesses) {}

  unsigned getNumObservations() const { return NumObservations; }
  unsigned getNumMemoryAccesses() const { return NumMemoryAccesses; }

  void print(llvm::raw_ostream &OS) const;
};

class MAIRStatAnalysis : public llvm::AnalysisInfoMixin<MAIRStatAnalysis> {
  friend llvm::AnalysisInfoMixin<MAIRStatAnalysis>;
  static llvm::AnalysisKey Key;

public:
  using Result = MAIRStat;

  MAIRStat run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

class MAIRStatPrinterPass : public llvm::PassInfoMixin<MAIRStatPrinterPass> {
  llvm::raw_ostream &OS;

public:
  explicit MAIRStatPrinterPass(llvm::raw_ostream &OS) : OS(OS) {}
  static bool isRequired() { return true; }

  llvm::PreservedAnalyses run(MAIRGraph &G, MAIRGraphAnalysisManager &AM);
};

} // namespace mair
