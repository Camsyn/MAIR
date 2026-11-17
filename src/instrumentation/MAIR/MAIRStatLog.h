#pragma once

#include "llvm/ADT/Statistic.h"
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
  // Loop aggregates whose trip count is read off induction variables at the
  // loop exit (a subset of range and periodic).
  ExitTrip,
  // Why a loop pass left an observation inside its loop: a barrier in the
  // loop, an address that is no affine recurrence of the loop, a source that
  // may skip an iteration, more than one exit, no trip count, anything else.
  RejectBarrier,
  RejectNonAffine,
  RejectConditional,
  RejectMultiExit,
  RejectNoCount,
  RejectOther,
  // Sanitizer selection: observations lowered through MAIR or left to native
  // lowering, why each native one fell back, and the source accesses that
  // fall back.
  Lowered,
  Native,
  Multi,
  Vector,
  Symbolic,
  Shared,
  Fallback,
  // Object bounds elimination: whether proving an eliminated observation
  // needed LazyValueInfo after ScalarEvolution.
  Scev,
  Lvi,
};

/// Whether `-mair-stats` was requested. Independent of LLVM `-stats`.
bool mairStatsEnabled();

/// Remember which default pipeline produced the counts. First call wins.
void noteMAIRPipeline(llvm::StringRef Pipeline);

/// One visit of a pass to one graph, reported by `-mair-stats`. Nested graph
/// visits are sequential, not re-entrant.
class MAIRStatScope {
public:
  /// \p CountInputs returns what the pass starts from: the observations, or
  /// for graph simplification the program points, of \p G. Counting walks the
  /// graph, so it happens only if something consumes the count: \p InputStat
  /// when LLVM statistics are compiled in, or `-mair-stats`.
  template <class StatT, class CountFn>
  MAIRStatScope(llvm::StringRef Pass, unsigned Ordinal, const MAIRGraph &G,
                StatT &InputStat, CountFn &&CountInputs) {
    const bool Reported = mairStatsEnabled();
    if (!LLVM_ENABLE_STATS && !Reported)
      return;
    const uint64_t NumInputs = CountInputs();
    InputStat += NumInputs;
    if (Reported)
      begin(Pass, Ordinal, G, NumInputs);
  }
  ~MAIRStatScope();

  static void add(MAIRStatField Field, uint64_t N = 1);

  MAIRStatScope(const MAIRStatScope &) = delete;
  MAIRStatScope &operator=(const MAIRStatScope &) = delete;

private:
  static void begin(llvm::StringRef Pass, unsigned Ordinal, const MAIRGraph &G,
                    uint64_t NumInputs);
};

} // namespace mair
