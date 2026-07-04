#include "MAIR/Analysis/MAIRStatAnalysis.h"

#include "llvm/IR/Function.h"

using namespace llvm;

namespace mair {

AnalysisKey MAIRStatAnalysis::Key;

void MAIRStat::print(raw_ostream &OS) const {
  OS << "  observations: " << NumObservations << "\n";
  OS << "  memory-accesses: " << NumMemoryAccesses << "\n";
}

MAIRStat MAIRStatAnalysis::run(MAIRGraph &G, MAIRGraphAnalysisManager &) {
  unsigned NumMemoryAccesses = 0;
  for (const Observation &Obs : G)
    NumMemoryAccesses += Obs.size();

  return MAIRStat(G.size(), NumMemoryAccesses);
}

PreservedAnalyses MAIRStatPrinterPass::run(MAIRGraph &G,
                                           MAIRGraphAnalysisManager &AM) {
  OS << "MAIRStat";
  if (const Function *F = G.getFunction())
    OS << " for function: " << F->getName();
  OS << "\n";
  AM.getResult<MAIRStatAnalysis>(G).print(OS);

  return PreservedAnalyses::all();
}

} // namespace mair
