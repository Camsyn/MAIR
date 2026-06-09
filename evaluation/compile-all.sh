#! /bin/bash

# -----------------------------------------------------------------------------
# Script description:
#   This script batch-builds benchmark programs under a package directory for multiple Sanitizer modes.
#   It iterates all supported modes and calls the package-local compile.sh for each mode.
#   It supports forcing rebuilds (-f) and custom package paths/build modes.
#   It also supports -t for compile-time tracing (-ftime-trace).
#
# Usage:
#   ./compile-all.sh [package-path] [-f] [-t] [mode1 mode2 ...]
#
# Arguments can appear in any order:
#   [package-path]    : optional package directory containing compile.sh; defaults to current directory
#   -f                : optional force rebuild, even if artefacts already exist
#   -t                : optional compile-time tracing by exporting -ftime-trace
#   [mode1 mode2 ...] : optional modes to build (asan msan raw ...); if omitted, build all supported modes
#
# Example:
#   ../compile-all.sh
#   ./compile-all.sh ./binutils
#   ../compile-all.sh -f
#   ./compile-all.sh ./binutils -f
#   ./compile-all.sh ./binutils asan msan
#   ../compile-all.sh -f asan msan
#   ./compile-all.sh ./binutils asan -f
#   ./compile-all.sh ./binutils -t
#   ../compile-all.sh -f -t asan msan
#
# Typical use:
#   Build all or selected Sanitizer configurations for later evaluation and testing.
# -----------------------------------------------------------------------------


# Available modes
modes=(
    raw
    asan
    ubsan
    tsan
    msan
    # dbg
    # xsan-asan
    # xsan-asan-tsan
    # xsan-asan-msan
    # xsan-asan-ubsan
    # xsan-asan-tsan-ubsan
    xsan-asan-msan-tsan
    # xsan-asan-msan-ubsan
    # xsan-asan-msan-tsan-ubsan
)

enabled_modes=( )

force=0
workdir=""
track_time=0



# Parse arguments: support -f, -t, <package-path> in any order
while [[ $# -gt 0 ]]; do
    case "$1" in
        -f)
            force=1
            shift
            ;;
        -t)
            track_time=1
            shift
            ;;
        *)
            # If workdir is empty and $1 is a directory, treat $1 as workdir
            if [ -z "$workdir" ] && [ -e "$1" ]; then
                workdir="$1"
            else
                enabled_modes+=("$1")
            fi
            shift
            ;;
    esac
done

# If enabled_modes is empty, fall back to all modes
if [ ${#enabled_modes[@]} -eq 0 ]; then
    enabled_modes=("${modes[@]}")
fi

if [ -z "$workdir" ]; then
    workdir="$(pwd)"
fi

if [ ! -f "$workdir/compile.sh" ]; then
    echo "Error: $workdir/compile.sh not found"
    echo "Please run this script in a directory containing compile.sh, or pass that directory as the first argument."
    exit 1
fi

pushd $workdir

# Ensure repo directory exists and is non-empty; otherwise call package/fetch.sh
if [ ! -d repo ] || [ -z "$(ls -A repo 2>/dev/null)" ]; then
    if [ -f "$workdir/fetch.sh" ]; then
        echo "Repo directory missing or empty, calling $workdir/fetch.sh to fetch sources..."
        bash "$workdir/fetch.sh"
    else
        echo "Error: $workdir/fetch.sh not found, cannot fetch sources"
        exit 1
    fi
fi


# Derive package name from directory name
package=$(basename "$workdir")

# If track_time=1, export related environment variables
if [ $track_time -eq 1 ]; then
    export CFLAGS="-ftime-trace ${CFLAGS}"
    export CXXFLAGS="-ftime-trace ${CXXFLAGS}"
fi

for mode in "${enabled_modes[@]}"; do
    echo "=============================="
    echo "Compile mode: $mode"
    skip=1
    if [ $force -eq 1 ]; then
        skip=0
    else
            # Check whether .compiled/<mode> already exists
        marker=".compiled/$mode"
        if [ -f "$marker" ]; then
            skip=1
        else
            skip=0
        fi
    fi

    if [ $skip -eq 1 ]; then
        echo "All artefacts for $mode already exist, skipping compilation"
        continue
    fi

    export mode="$mode"
    start_time=$(date +%s)
    ./compile.sh
    if [ $? -ne 0 ]; then
        echo "Compilation failed, skipping $mode"
        continue
    fi
    end_time=$(date +%s)
    compile_time=$((end_time - start_time))

    # Check whether temp/.bin exists
    if [ ! -d temp/.bin ]; then
        echo "Warning: temp/.bin directory does not exist, skipping $mode"
        continue
    fi
    for bin in temp/.bin/*; do
        [ -f "$bin" ] || continue
        prog=$(basename "$bin")
        mkdir -p artefacts/"$prog"
        mv "$bin" "artefacts/$prog/$prog.$mode"
        # Mark artefacts as compiled
        echo "Moved $bin to artefacts/$prog/$prog.$mode"
    done
    mkdir -p ".compiled/"
    echo "Compile Time: $compile_time s" > ".compiled/$mode"

    # In trace mode, collect *.json compile-time trace files
    if [ $track_time -eq 1 ]; then
        data_dir=".compiled/data-time-trace/$mode"
        rm -rf "$data_dir" && mkdir -p "$data_dir"
        # Find all *.json files
        found=0
        for f in $(find ./temp -type f -name "*.json" 2>/dev/null); do
            found=1
            # Use cp --parents to preserve path structure
            cp --parents "$f" "$data_dir" 2>/dev/null || \
                (mkdir -p "$(dirname "$data_dir/$f")" && cp "$f" "$data_dir/$f")
        done
        if [ $found -eq 1 ]; then
            # Move $data_dir/temp/* to $data_dir/
            mv "$data_dir/temp/*" "$data_dir/"
            rm -rf "$data_dir/temp"
            echo "Collected $mode compile trace files into $data_dir/"
        else
            echo "No $mode compile trace (.json) files found"
        fi
    fi
done

popd
