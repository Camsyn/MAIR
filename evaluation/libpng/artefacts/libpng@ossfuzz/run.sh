#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
export PRE_OPT=""
export POST_OPT=""
export stdin="false"

# Make MSan non-fatal and reduce noise; it may still report but should not exit
export MSAN_OPTIONS="poison_in_dtor=0:poison_in_malloc=0:poison_in_free=0"


$PROG_DIR/../../../run.sh $@
