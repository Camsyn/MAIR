#include "Options.h"

namespace __xsan {
namespace options {

namespace opt {
const cl::opt<bool> ClOpt("xsan-opt", cl::desc("Optimize instrumentation"),
                          cl::Hidden, cl::init(true));
const cl::opt<bool> ClReccReduce("xsan-reduce-rec",
                                 cl::desc("Reduce recurring checks"),
                                 cl::Hidden, cl::init(true));
const cl::opt<bool>
    ClReccReduceAsan("xsan-reduce-rec-asan",
                     cl::desc("Reduce recurring checks for ASan"), cl::Hidden,
                     cl::init(true));
const cl::opt<bool>
    ClReccReduceTsan("xsan-reduce-rec-tsan",
                     cl::desc("Reduce recurring checks for TSan"), cl::Hidden,
                     cl::init(true));

/// TODO: The current analysis is still wrong, and we should not optimize the
/// inspection of Data Race based on the assumption that Data Race does not
/// exist.
/// Maybe we could optimize the tail checking based on the assumption of 
/// Use-After-Return does not exist.
const cl::opt<bool>
    ClTsanOptStackObj("xsan-tsan-opt-stack-obj",
                      cl::desc("Optimize stack object for TSan"), cl::Hidden,
                      cl::init(false));

const cl::opt<LoopOptLeval> ClLoopOpt(
    "xsan-loop-opt", cl::desc("Loop optimization level for XSan"),
    cl::values(
        clEnumValN(LoopOptLeval::NoOpt, "no",
                   "Disable loop optimization for XSan"),
        clEnumValN(LoopOptLeval::CombineToRangeCheck, "range",
                   "Only combine periodic checks to range check for XSan"),
        clEnumValN(LoopOptLeval::CombinePeriodicChecks, "period",
                   "Only combine periodic checks for XSan"),
        clEnumValN(LoopOptLeval::RelocateInvariantChecks, "invariant",
                   "Only relocate invariant checks for XSan"),
        clEnumValN(LoopOptLeval::Full, "full",
                   "Enable all loop optimization for XSan")),
    cl::Hidden, cl::init(LoopOptLeval::Full));

const cl::opt<bool> ClPostOpt(
    "xsan-post-opt", cl::init(true),
    cl::desc("Whether to perform post-sanitziers optimizations for XSan"),
    cl::Hidden);

} // namespace opt

const cl::opt<bool> ClDisableAsan("xsan-disable-asan", cl::init(false),
                                  cl::desc("Disable ASan instrumentation"),
                                  cl::Hidden);

const cl::opt<bool> ClDisableTsan("xsan-disable-tsan",
                                  cl::init(false),
                                  cl::desc("Disable TSan instrumentation"),
                                  cl::Hidden);

const cl::opt<bool> ClDisableMsan("xsan-disable-msan",
                                  cl::init(false),
                                  cl::desc("Disable MSan instrumentation"),
                                  cl::Hidden);

cl::opt<bool> ClDebug("xsan-debug", cl::init(false),
                      cl::desc("Enable debug output for XSan"), cl::Hidden);

cl::opt<std::string> ClDumpDeSanReSan(
    "dump-desan-resan", cl::init(""), cl::value_desc("dir"),
    cl::desc("Dump DeSan/ReSan IR snapshots into the given directory"),
    cl::Hidden);
} // namespace options

} // namespace __xsan
