#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
# Common nm-new options include -A, -g, -n and -C, used to display symbol table information.
export PRE_OPT=""
export POST_OPT=""
export stdin="false"

$PROG_DIR/../../../run.sh $@
