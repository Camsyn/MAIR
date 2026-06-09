#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
# objcopy usually takes input/output files through command-line arguments rather than stdin
# The most common option is -O <target-format>, for example -O binary
export PRE_OPT="-O binary"
export POST_OPT="/tmp/objcopy.out"
export stdin="false"

$PROG_DIR/../../../run.sh $@
