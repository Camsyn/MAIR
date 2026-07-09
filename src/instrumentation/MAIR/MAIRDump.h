#pragma once

#include "MAIR/MAIR.h"

#include "llvm/ADT/ArrayRef.h"

namespace mair {

// Dump MAIR as MAIR format
void dumpMAIR(const MAIRGraph &Graph, raw_ostream &OS);
// Dump MAIR graph as Graphviz DOT.
void dumpDOT(const MAIRGraph &Graph, raw_ostream &OS);
// Dump MAIR of function as LLVM format
void dumpLLVM(const MAIRGraph &Graph, raw_ostream &OS);
// Dump MAIR of module as LLVM format
void dumpLLVM(ArrayRef<const MAIRGraph *> Graphs, raw_ostream &OS);

} // namespace mair
