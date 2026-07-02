#pragma once

#include "MAIR/MAIR.h"

namespace mair {

void dumpPure(const MAIRGraph &Graph, raw_ostream &OS);
void dumpAnnotated(const MAIRGraph &Graph, const Module &M, raw_ostream &OS);

} // namespace mair
