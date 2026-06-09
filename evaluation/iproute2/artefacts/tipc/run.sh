#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)

# TIPC tool setup: still useful for sanitizer overhead even when the kernel module is not loaded
export stdin="false"
export MSAN_OPTIONS="poison_in_free=0:poison_in_malloc=0"

# Use simple help arguments; sanitizer overhead can still be measured even if the command reports an error
export PRE_OPT=""
export POST_OPT="-h"

$PROG_DIR/../../../run.sh $@

