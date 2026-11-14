#pragma once

#include "MAIR/MAIR.h"

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class ModuleSlotTracker;
} // namespace llvm

namespace mair {

// Dump MAIR as MAIR format
void dumpMAIR(const MAIRGraph &Graph, raw_ostream &OS);
// The same, numbering unnamed values with \p MST. Pass one tracker for all
// graphs of a module: each call without one numbers the whole module.
void dumpMAIR(const MAIRGraph &Graph, raw_ostream &OS,
              llvm::ModuleSlotTracker &MST);
// Dump MAIR graph as Graphviz DOT.
void dumpDOT(const MAIRGraph &Graph, raw_ostream &OS);
// Dump MAIR of function as LLVM format
void dumpLLVM(const MAIRGraph &Graph, raw_ostream &OS);
// Dump MAIR of module as LLVM format
void dumpLLVM(ArrayRef<const MAIRGraph *> Graphs, raw_ostream &OS);

} // namespace mair
