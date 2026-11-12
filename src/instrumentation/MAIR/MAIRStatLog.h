#pragma once

#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace mair {

class MAIRGraph;

/// The counters a pass can report. `-mair-stats` prints them in this order.
enum class MAIRStatField : uint8_t {
  In,
  Preheader,
  Exit,
  Range,
  Periodic,
  Merged,
  Intra,
  Inter,
  Eliminated,
  Hoisted,
  Sunk,
};

/// Remember which default pipeline produced the counts. First call wins.
void noteMAIRPipeline(llvm::StringRef Pipeline);

/// One visit of a pass to one graph, reported by `-mair-stats` (which is
/// independent of LLVM `-stats`). Nested graph visits are sequential, not
/// re-entrant.
class MAIRStatScope {
public:
  /// \p NumInputs is what the pass starts from: the observations, or for
  /// graph simplification the program points, of \p G.
  MAIRStatScope(llvm::StringRef Pass, unsigned Ordinal, const MAIRGraph &G,
                uint64_t NumInputs);
  ~MAIRStatScope();

  static void add(MAIRStatField Field, uint64_t N = 1);

  MAIRStatScope(const MAIRStatScope &) = delete;
  MAIRStatScope &operator=(const MAIRStatScope &) = delete;
};

} // namespace mair
