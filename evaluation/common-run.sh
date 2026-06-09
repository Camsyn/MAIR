#!/bin/bash

# -----------------------------------------------------------------------------
# Script description:
#   This script runs a selected benchmark program under a given Sanitizer mode.
#   Arguments select the mode, program directory, input style (stdin or file), and extra options.
#
# Usage:
#   ./common-run.sh <mode> <program-dir> <stdin:bool> [<input>] [preoptions] [postoptions]
#
# Arguments:
#   <mode>           : runtime mode (asan, msan, xsan-asan, etc.; must match build mode)
#   <program-dir>    : directory containing program artefacts (for example ./artefacts/foo)
#   <stdin:bool>     : whether to pass input through stdin (true/false)
#   <input>          : optional input file path
#   preoptions       : optional program arguments placed before the input file
#   postoptions      : optional program arguments placed after the input file
#
# Example:
#   ./common-run.sh asan ./artefacts/foo false ./inputs/foo/input1 -v
#   ./common-run.sh xsan-asan ./artefacts/bar true ./inputs/bar/input2
#
# Typical use:
#   Called by /run.sh to run benchmark programs consistently across Sanitizer modes.
# -----------------------------------------------------------------------------


# Usage help
usage() {
    echo "Usage: $0 <mode> <program-dir> <use-perf:bool> <stdin:bool> [<input>] [preoptions] [postoptions]"
    echo "  <mode>           : run mode (e.g., asan, msan, xsan-asan, ...)"
    echo "  <program-dir>    : directory where the program artefacts reside"
    echo "  <use-perf:bool>  : whether to measure CPU + memory performance (true/false)"
    echo "  <stdin:bool>     : whether to feed input via stdin (true/false)"
    echo "  <input>          : (optional) input file path"
    echo "  preoptions       : (optional) arguments before the input"
    echo "  postoptions      : (optional) arguments after the input"
    echo "Examples:"
    echo "  $0 asan ./artefacts/foo false ./inputs/foo/input1 -v"
    echo "  $0 xsan-asan ./artefacts/bar true ./inputs/bar/input2"
    exit 1
}

# Check argument count
if [ $# -lt 3 ]; then
    echo "Error: insufficient arguments."
    usage
fi

export ASAN_OPTIONS=detect_leaks=0
export TSAN_OPTIONS=report_bugs=0

SCRIPTS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd)
PERF_SCRIPT=$SCRIPTS_DIR/eval-sample.sh

mode="$1"
program_path="$2"/$(basename "$2")
use_perf="$3"
stdin="$4"
shift 4

# Validate stdin flag
if [[ "$stdin" != "true" && "$stdin" != "false" ]]; then
    echo "Error: <stdin:bool> must be true or false."
    usage
fi

# Check program binary existence
prog_bin="${program_path}.${mode}"
if [ ! -f "$prog_bin" ]; then
    echo "Error: program file $prog_bin does not exist."
    exit 2
fi

# Handle input / preoptions / postoptions
input=""
preoptions=""
postoptions=""
if [ $# -ge 3 ]; then
    input="$1"
    preoptions="$2"
    postoptions="$3"
    shift 3
elif [ $# -ge 2 ]; then
    input="$1"
    preoptions="$2"
    shift 2
elif [ $# -ge 1 ]; then
    input="$1"
    shift 1
fi

# If an input file is specified, ensure it exists
if [ -n "$input" ] && [ ! -f "$input" ]; then
    echo "Error: input file $input does not exist."
    exit 3
fi


if [ "$stdin" = "false" ]; then
    # If input exists, place it between preoptions and postoptions
    if [ -n "$input" ]; then
        cmd=( "$prog_bin" $preoptions "$input" $postoptions )
    else
        cmd=( "$prog_bin" $preoptions $postoptions )
    fi
    if [ "$use_perf" = "true" ]; then
        cmd=( "$PERF_SCRIPT" "${cmd[@]}" )
    fi
    
    # echo "Running command: ${cmd[@]}"
    exec ${cmd[@]}
else
    # stdin mode
    cmd=( "$prog_bin" $preoptions $postoptions )
    if [ "$use_perf" = "true" ]; then
        cmd=( "$PERF_SCRIPT" "${cmd[@]}" )
    fi
    if [ -n "$input" ]; then
        "${cmd[@]}" < "$input"
    else
        "${cmd[@]}"
    fi
fi
