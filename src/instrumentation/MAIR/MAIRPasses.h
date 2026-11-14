#pragma once

#include "MAIR/MAIR.h"

#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Error.h"

namespace llvm {
template <>
void printIRUnitNameForStackTrace<mair::MAIRGraph>(raw_ostream &OS,
                                                   const mair::MAIRGraph &G);
} // namespace llvm

namespace mair {

using MAIRGraphAnalysisManager = llvm::AnalysisManager<MAIRGraph>;
using MAIRGraphPassManager =
    llvm::PassManager<MAIRGraph, MAIRGraphAnalysisManager>;

void registerMAIRGraphAnalyses(MAIRGraphAnalysisManager &AM,
                               llvm::PassInstrumentationCallbacks *PIC,
                               llvm::FunctionAnalysisManager *FAM = nullptr);

bool normalizeMAIRLoops(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);

void buildMAIRGraphOgPipeline(MAIRGraphPassManager &PM);
void buildMAIRGraphO1Pipeline(MAIRGraphPassManager &PM);
/// The pipeline MAOpt runs: Og when \p KeepFullDebug, else O1, unless the
/// hidden `-mair-pipeline=<text>` option replaces it.
void buildMAIRGraphDefaultPipeline(MAIRGraphPassManager &PM,
                                   bool KeepFullDebug);

llvm::Error parseMAIRGraphPassPipeline(MAIRGraphPassManager &PM,
                                       llvm::StringRef PipelineText);

} // namespace mair
