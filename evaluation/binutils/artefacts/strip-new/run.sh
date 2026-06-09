#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
# The most common strip option is to specify the output file using -o.
export PRE_OPT=""
export POST_OPT="-o /tmp/strip-out"
export stdin="false"

$PROG_DIR/../../../run.sh $@
