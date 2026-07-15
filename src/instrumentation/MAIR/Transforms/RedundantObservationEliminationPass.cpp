#include "MAIR/Transforms/RedundantObservationEliminationPass.h"
#include "MAIR/Analysis/RedundantObservationAnalysis.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

using namespace llvm;

#define DEBUG_TYPE "mair-redundant-observation-elimination"

STATISTIC(NumEliminatedAccesses, "Number of redundant accesses eliminated");

namespace mair {

PreservedAnalyses
RedundantObservationEliminationPass::run(MAIRGraph &G,
                                         MAIRGraphAnalysisManager &AM) {
  const RedundantObservationInfo &Info =
      AM.getResult<RedundantObservationAnalysis>(G);
  DenseSet<const MemoryAccess *> DeletedSources;
  SmallVector<MemoryAccess *, 16> Redundant;
  for (Observation &Obs : G) {
    const AvailableAccessSet &In = Info.getIn(Obs);
    for (MemoryAccess &Access : Obs) {
      if (!In.covers(Access, DeletedSources))
        continue;
      DeletedSources.insert(&Access);
      Redundant.push_back(&Access);
    }
  }

  for (MemoryAccess *Access : Redundant) {
    Observation *Parent = Access->getParent();
    auto It = llvm::find_if(Parent->accesses(), [&](MemoryAccess &Candidate) {
      return &Candidate == Access;
    });
    assert(It != Parent->end() && "redundant access must belong to parent");
    Parent->erase(It);
  }
  NumEliminatedAccesses += Redundant.size();
  return Redundant.empty() ? PreservedAnalyses::all()
                           : PreservedAnalyses::none();
}

} // namespace mair
