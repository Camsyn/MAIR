#pragma once

#include "llvm/ADT/StringRef.h"

namespace mair {

class MAIRGraph;

/// Whether `-mair-stats` was requested. Independent of LLVM `-stats`.
bool mairStatsEnabled();

/// Remember which default pipeline produced the counts. First call wins.
void noteMAIRPipeline(llvm::StringRef Pipeline);

/// Function name of the root graph. Child loop graphs do not store it.
llvm::StringRef mairStatFunctionName(const MAIRGraph &G);

/// One pass visit. Nested graph visits are sequential, not re-entrant.
class MAIRStatScope {
public:
  MAIRStatScope(llvm::StringRef Pass, unsigned Ordinal, llvm::StringRef Func);
  ~MAIRStatScope();

  static void add(llvm::StringRef Field, uint64_t N = 1);

  MAIRStatScope(const MAIRStatScope &) = delete;
  MAIRStatScope &operator=(const MAIRStatScope &) = delete;
};

} // namespace mair
