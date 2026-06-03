#include "Options.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Support/ErrorHandling.h>

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

cl::list<std::string>
    ClSanitizers("xsan-sanitizers", cl::CommaSeparated, cl::ZeroOrMore,
                 cl::value_desc("asan,tsan,msan|none"),
                 cl::desc("Explicitly select XSan sub-sanitizers"), cl::Hidden);

bool isSelectedSanitizer(llvm::StringRef Name) {
  if (ClSanitizers.empty())
    llvm::report_fatal_error("XSan requires explicit -xsan-sanitizers="
                             "asan,tsan,msan selection");
  if (Name != "asan" && Name != "tsan" && Name != "msan")
    llvm::report_fatal_error(llvm::Twine("unknown XSan sanitizer name: ") +
                             Name);

  bool Selected = false;
  bool HasNone = false;
  for (const std::string &San : ClSanitizers) {
    llvm::StringRef Value(San);
    if (Value.empty())
      continue;
    if (Value == "none") {
      HasNone = true;
      continue;
    }
    if (Value != "asan" && Value != "tsan" && Value != "msan")
      llvm::report_fatal_error(llvm::Twine("unknown -xsan-sanitizers entry: ") +
                               Value);
    Selected |= Value == Name;
  }
  if (HasNone && Selected)
    llvm::report_fatal_error("-xsan-sanitizers=none cannot be combined with "
                             "other sanitizer names");
  return Selected;
}

cl::opt<bool> ClDebug("xsan-debug", cl::init(false),
                      cl::desc("Enable debug output for XSan"), cl::Hidden);

cl::opt<std::string>
    ClDumpDeSanReSan("xsan-dump", cl::init(""), cl::value_desc("dir"),
                     cl::desc("Dump the whole XSan instrumentation pipeline "
                              "into the given directory"),
                     cl::Hidden);

// UBSan sanitizing is emitted by the compiler frontend. Later optimizations may
// break part of the sanitizing semantics, which makes
// sanitizer-assumption-based DeSan/ReSan hard to handle well for UBSan.
// Therefore, full UBSan DeSan/ReSan is disabled by default because this path
// hides many optimization-induced bugs.
//
// Ideal situation   : (San -> DeSan)* -> ReSan
// UBSan's situation : UBSan -> Opt -> DeSan -> (San -> DeSan)* -> ReSan
cl::opt<bool> ClDeUBSan(
    "xsan-de-ubsan", cl::init(false),
    cl::desc(
        "Enable full UBSan DeSan/ReSan handling beyond arithmetic recovery"),
    cl::Hidden);
} // namespace options

} // namespace __xsan
