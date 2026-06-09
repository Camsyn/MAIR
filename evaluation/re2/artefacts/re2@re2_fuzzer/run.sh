#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
export PRE_OPT=""
export POST_OPT=""
export stdin="true"

export MSAN_OPTIONS="poison_in_malloc=0:poison_in_free=0:poison_in_dtor=0:report_umrs=0:halt_on_error=0:exitcode=0:print_summary=false:verbosity=0"

# UBSan: silence diagnostics and force zero exit code
# export UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=0:exitcode=0:${UBSAN_OPTIONS}"

# ASan/TSan: also silence them and make them exit with code 0
# export ASAN_OPTIONS="halt_on_error=0:exitcode=0:detect_leaks=0:allocator_may_return_null=1:print_summary=false:${ASAN_OPTIONS}"
# export TSAN_OPTIONS="halt_on_error=0:exitcode=0:report_signal_unsafe=0:${TSAN_OPTIONS}"
"$PROG_DIR"/../../../run.sh "$@"
