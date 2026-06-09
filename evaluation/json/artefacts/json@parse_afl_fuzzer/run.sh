#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
export PRE_OPT=""
export POST_OPT=""
export stdin="true"

# This is a C++ project. Since we do not instrument libc++, MSan may report
# false positives. To avoid those, temporarily disable poisoning in malloc,
# free and destructors so MSan does not touch shadow memory for heap ops.
export MSAN_OPTIONS="poison_in_malloc=0:poison_in_free=0:poison_in_dtor=0"

$PROG_DIR/../../../run.sh $@
