#pragma once

#include "MAIR/MAIR.h"

namespace mair {

void dumpMAIR(const MAIRGraph &Graph, raw_ostream &OS);
void dumpLLVM(const MAIRGraph &Graph, const Module &M, raw_ostream &OS);

} // namespace mair
