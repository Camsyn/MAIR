#!/bin/bash

# -----------------------------------------------------------------------------
# Script description:
#   This script provides a shared entry point for benchmark programs and calls common-run.sh.
#   Environment variables and arguments select Sanitizer mode, input file, and pre/post options.
#
# Usage: called from <package>/artefacts/<program>/run.sh
#   /path/to/this/run.sh <mode> [<input>]
#
#   <mode>   : run mode (e.g., raw, asan, msan, xsan-asan; must match build)
#   <input>  : (optional) input file path; if omitted, common-run.sh decides
#
# Required environment variables:
#   PRE_OPT   : (optional) program arguments placed before the input file
#   POST_OPT  : (optional) program arguments placed after the input file
#   stdin     : (required) whether to pipe input via stdin (true/false)
#   PROG_DIR  : (required) artefacts directory of the program
#
# Examples:
#   PRE_OPT="-v" POST_OPT="--debug" stdin=false PROG_DIR=./artefacts/foo ./run.sh asan ./inputs/foo/input1
#   stdin=true PROG_DIR=./artefacts/bar ./run.sh xsan-asan ./inputs/bar/input2
# -----------------------------------------------------------------------------


# Generic run wrapper

# Ensure required variables are defined (may be empty for PRE_OPT/POST_OPT)
: "${PRE_OPT?PRE_OPT must be defined}"
: "${POST_OPT?POST_OPT must be defined}"
# These must not be empty
: "${stdin:?stdin must be defined}"
: "${PROG_DIR:?PROG_DIR must be defined}"

usage() {
    echo "Usage: $PROG_DIR/run.sh <mode> [<input>]"
    exit 1
}

if [ $# -lt 1 ]; then
    usage
fi

FILE_DIR=$(cd "$(dirname "$0")"; pwd)

mode="$1"
shift
input="${1:-}"
# If stdin=true and no input is provided, pick a default input file from corpus
if [ "$stdin" = "true" ] && [ -z "$input" ]; then
    # Derive program name from PROG_DIR
    prog_name=$(basename "$PROG_DIR")

    # Build corpus directory path
    corpus_dir="$PROG_DIR/../../corpus/$prog_name"

    # If corpus directory exists, take the first file as default input
    if [ -d "$corpus_dir" ]; then
        # Get the first file in the corpus directory
        first_file=$(find "$corpus_dir" -type f | head -n 1)
        if [ -n "$first_file" ]; then
            input="$first_file"
        fi
    fi
fi

if [ -n "$USE_PERF" ]; then
    use_perf="true"
else
    use_perf="false"
fi


"$FILE_DIR/common-run.sh" "$mode" "$PROG_DIR" "$use_perf" "$stdin" "$input" "$PRE_OPT" "$POST_OPT"
