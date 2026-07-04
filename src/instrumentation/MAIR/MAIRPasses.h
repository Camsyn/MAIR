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
                               llvm::PassInstrumentationCallbacks *PIC);

llvm::Error parseMAIRGraphPassPipeline(MAIRGraphPassManager &PM,
                                       llvm::StringRef PipelineText);

} // namespace mair
