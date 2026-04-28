#include "../Instrumentation.h"
#include <llvm/Support/CommandLine.h>

#include <string>
namespace __xsan {

/// TODO: Add and move all options here.

namespace options {
using namespace llvm;

namespace opt {
/// The master switch for XSan's instrumentation.
extern const cl::opt<bool> ClOpt;

/// Whether to reduce the recurring checks.
extern const cl::opt<bool> ClReccReduce;
/// Whether to reduce the recurring checks for ASan.
extern const cl::opt<bool> ClReccReduceAsan;
/// Whether to reduce the recurring checks for TSan.
extern const cl::opt<bool> ClReccReduceTsan;
/// Whether to optimize the instrumentation of stack objects for TSan.
extern const cl::opt<bool> ClTsanOptStackObj;

/// The level of loop optimization designated for XSan.
/// - no: No loop optimization.
/// - range: Only combine periodic checks to range check.
/// - period: Only combine periodic checks.
/// - invariant: Only relocate invariant checks.
/// - full: Enable all loop optimizations as above.
extern const cl::opt<LoopOptLeval> ClLoopOpt;

/// Whether to perform post-optimization.
extern const cl::opt<bool> ClPostOpt;

inline bool enableReccReduction() { return ClOpt && ClReccReduce; }

inline bool enableReccReductionAsan() {
  return enableReccReduction() && ClReccReduceAsan;
}
inline bool enableReccReductionTsan() {
  return enableReccReduction() && ClReccReduceTsan;
}

inline bool enableTsanOptStackObj() { return ClOpt && ClTsanOptStackObj; }

inline bool enablePostOpt() { return ClOpt && ClPostOpt; }

inline LoopOptLeval loopOptLevel() {
  return ClOpt ? ClLoopOpt : LoopOptLeval::NoOpt;
}
} // namespace opt

/// Disable TSan instrumentation.
extern const cl::opt<bool> ClDisableTsan;

/// Disable ASan instrumentation.
extern const cl::opt<bool> ClDisableAsan;

/// Disable MSan instrumentation.
extern const cl::opt<bool> ClDisableMsan;

/// Enable debug output.
extern cl::opt<bool> ClDebug;

/// Dump the DeSan/ReSan pipeline IR snapshots into the given directory.
extern cl::opt<std::string> ClDumpDeSanReSan;

inline bool enableDeSanReSanDump() { return !ClDumpDeSanReSan.empty(); }

} // namespace options

} // namespace __xsan
