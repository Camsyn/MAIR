#pragma once

#include "MAIR/MAIRPasses.h"

#include <functional>
#include <type_traits>

template <class GraphType> auto testProgramPoints(GraphType &Graph) {
  using PointType =
      std::conditional_t<std::is_const_v<GraphType>, const mair::ProgramPoint,
                         mair::ProgramPoint>;
  llvm::SmallVector<std::reference_wrapper<PointType>, 16> Points;
  for (auto *Point : Graph.allProgramPoints())
    Points.emplace_back(*Point);
  return Points;
}

template <class PassType>
llvm::PreservedAnalyses runPassOnGraphTree(PassType &&Pass,
                                           mair::MAIRGraph &Graph,
                                           mair::MAIRGraphAnalysisManager &AM) {
  llvm::PreservedAnalyses Result = llvm::PreservedAnalyses::all();
  auto Graphs = Graph.graphs();
  for (mair::MAIRGraph *Current : llvm::reverse(Graphs)) {
    llvm::PreservedAnalyses Preserved = Pass.run(*Current, AM);
    AM.invalidate(*Current, Preserved);
    Result.intersect(Preserved);
  }
  return Result;
}

template <class BarrierTrait = mair::DefaultBarrierTrait>
mair::MAIRGraph
createGraphWithFunctionAnalyses(llvm::Function &Function,
                                llvm::FunctionAnalysisManager &FAM,
                                mair::MAIROptimizationConfig Config =
                                    mair::MAIROptimizationConfig::generic()) {
  mair::normalizeMAIRLoops(Function, FAM);
  return mair::MAIRGraph::build<BarrierTrait>(Function, FAM, Config);
}
