#!/bin/bash
export PROJ=libjpeg-turbo
URL="https://github.com/libjpeg-turbo/libjpeg-turbo.git"

# Use depth 1 to avoid cloning the history
git clone "$URL" repo --depth 1

# Pull fuzzer code from the fuzzbench git repository
echo "Fetching fuzzer code from fuzzbench git..."
if [ ! -d "fuzzbench-temp" ]; then
    git clone https://github.com/google/fuzzbench.git fuzzbench-temp --depth 1
else
    pushd fuzzbench-temp
    git pull origin main
    popd
fi

# Copy fuzzer-related files into repo
cp "fuzzbench-temp/benchmarks/libjpeg-turbo_libjpeg_turbo_fuzzer/libjpeg_turbo_fuzzer.cc" repo/
cp "fuzzbench-temp/benchmarks/libjpeg-turbo_libjpeg_turbo_fuzzer/build.sh" repo/
cp "fuzzbench-temp/benchmarks/libjpeg-turbo_libjpeg_turbo_fuzzer/benchmark.yaml" repo/

# Clean up the temporary fuzzbench clone
rm -rf fuzzbench-temp

# Search the repository to build an initial corpus
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_libjpeg-turbo_corpus.sh" ]; then
    "$SCRIPT_DIR/create_libjpeg-turbo_corpus.sh" || true
else
    echo "Warning: Executable create_libjpeg-turbo_corpus.sh not found, skipping corpus generation" >&2
fi

