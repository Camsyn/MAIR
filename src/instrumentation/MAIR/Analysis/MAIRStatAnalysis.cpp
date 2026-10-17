#include "MAIR/Analysis/MAIRStatAnalysis.h"

#include "llvm/IR/Function.h"

using namespace llvm;

namespace mair {

AnalysisKey MAIRStatAnalysis::Key;

void MAIRStat::print(raw_ostream &OS) const {
  OS << "  program-points: " << NumProgramPoints << "\n";
  OS << "  observations: " << NumObservations << "\n";
}

MAIRStat MAIRStat::compute(const MAIRGraph &G) {
  unsigned NumObservations = 0;
  for (const ProgramPoint &PP : G)
    NumObservations += PP.size();

  return MAIRStat(G.size(), NumObservations);
}

MAIRStat MAIRStatAnalysis::run(MAIRGraph &G, MAIRGraphAnalysisManager &) {
  unsigned NumProgramPoints = 0;
  unsigned NumObservations = 0;
  for (const MAIRGraph *Scope : G.graphs()) {
    MAIRStat Local = MAIRStat::compute(*Scope);
    NumProgramPoints += Local.getNumProgramPoints();
    NumObservations += Local.getNumObservations();
  }
  return MAIRStat(NumProgramPoints, NumObservations);
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
