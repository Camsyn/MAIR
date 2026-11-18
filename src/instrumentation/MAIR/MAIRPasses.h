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

/// \p EliminateWithinTrustedObjects adds object bounds elimination (`obe`),
/// for a sanitizer that trusts some objects
/// (MAIROptimizationConfig::trustsObjects).
void buildMAIRGraphOgPipeline(MAIRGraphPassManager &PM,
                              bool EliminateWithinTrustedObjects = false);
void buildMAIRGraphO1Pipeline(MAIRGraphPassManager &PM,
                              bool EliminateWithinTrustedObjects = false);
void buildMAIRGraphO3Pipeline(MAIRGraphPassManager &PM,
                              bool EliminateWithinTrustedObjects = false);
/// A sanitizer must opt in to partial coverage even for a custom pipeline.
void buildMAIRGraphDefaultPipeline(MAIRGraphPassManager &PM, bool KeepFullDebug,
                                   bool EliminateWithinTrustedObjects = false,
                                   bool AllowPartialCoverage = false);

llvm::Error parseMAIRGraphPassPipeline(MAIRGraphPassManager &PM,
                                       llvm::StringRef PipelineText,
                                       bool AllowPartialCoverage = true);

} // namespace mair
