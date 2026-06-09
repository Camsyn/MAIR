#!/bin/bash

# -----------------------------------------------------------------------------
# Script description:
#   This script measures one run of a selected program for performance and memory usage.
#   It uses perf and /usr/bin/time to report CPU time (task-clock, msec) and maximum resident set size (RSS, KB).
#
# Usage:
#   ./eval-sample.sh <program> <args>...
#
#   <program> : path to the executable to test (e.g., ./run.sh)
#   <args>    : arguments passed to <program> (e.g., mode, input file, etc.)
#
# Example:
#   ./eval-sample.sh <package>/artefacts/<program>/run.sh asan ./inputs/foo/input1
#
# Output:
#   CPU time: <msec>
#   RSS     : <KB>
# -----------------------------------------------------------------------------


# Check argument count
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <program> [<args>, ...]"
    exit 1
fi


PROGRAM=$1
shift 1

# Ensure perf is installed
if ! command -v perf &> /dev/null; then
    echo "Error: perf is not installed. Please install perf." >&2
    exit 1
fi

# Ensure /usr/bin/time exists
if [ ! -x /usr/bin/time ]; then
    echo "Error: /usr/bin/time is not available." >&2
    exit 1
fi

export ASAN_OPTIONS=detect_leaks=0
export TSAN_OPTIONS=report_bugs=0

# Capture combined perf and time output; suppress program stdout
OUTPUT=$(perf stat -- /usr/bin/time -v "$PROGRAM" $@ 2>&1)


# Extract CPU time (task-clock, msec) from perf output
CPU_TIME=$(echo "$OUTPUT" | grep "task-clock" | awk '{print $1}')

# Extract maximum RSS (KB) from /usr/bin/time output
MAX_RSS=$(echo "$OUTPUT" | grep "Maximum resident set size" | awk '{print $6}')

# Print final result
echo "CPU time: ${CPU_TIME} msec"
echo "RSS     : ${MAX_RSS} KB"
