#!/bin/bash
set -e

# Match sqlite3/libjpeg-turbo: fetch sources with git and fuzzer files from FuzzBench

export PROJ=libpng
URL="https://github.com/glennrp/libpng.git"

rm -rf repo
git clone "$URL" repo --depth 1

echo "Fetching fuzzer files from fuzzbench..."
if [ ! -d "fuzzbench-temp" ]; then
  git clone https://github.com/google/fuzzbench.git fuzzbench-temp --depth 1
else
  pushd fuzzbench-temp
  git pull --ff-only origin master || git pull --ff-only origin main || true
  popd
fi

# Copy metadata from FuzzBench (benchmark: libpng_libpng_read_fuzzer); it does not include the .cc source
if [ -f "fuzzbench-temp/benchmarks/libpng_read_fuzzer/build.sh" ]; then
  cp "fuzzbench-temp/benchmarks/libpng_read_fuzzer/build.sh" repo/
fi
if [ -f "fuzzbench-temp/benchmarks/libpng_read_fuzzer/benchmark.yaml" ]; then
  cp "fuzzbench-temp/benchmarks/libpng_read_fuzzer/benchmark.yaml" repo/
fi

# Clean up the temporary fuzzbench clone
rm -rf fuzzbench-temp

# Also fetch the actual fuzzer source libpng_read_fuzzer.cc from oss-fuzz
echo "Fetching libpng_read_fuzzer.cc from oss-fuzz..."
if [ ! -d "oss-fuzz-temp" ]; then
  git clone https://github.com/google/oss-fuzz.git oss-fuzz-temp --depth 1
else
  pushd oss-fuzz-temp
  git pull --ff-only origin master || git pull --ff-only origin main || true
  popd
fi

if [ -f "oss-fuzz-temp/projects/libpng/libpng_read_fuzzer.cc" ]; then
  cp "oss-fuzz-temp/projects/libpng/libpng_read_fuzzer.cc" repo/
else
  echo "Warning: libpng_read_fuzzer.cc not found in oss-fuzz" >&2
fi

rm -rf oss-fuzz-temp

echo "fetch finished."

#!/bin/bash
set -e

# Optionally copy additional sources from fuzzbench to improve portability.
echo "Copying libpng sources from fuzzbench..."
if [ -d "fuzzbench-temp/benchmarks/libpng_libpng_read_fuzzer" ]; then
  cp -r fuzzbench-temp/benchmarks/libpng_libpng_read_fuzzer/* repo/
  echo "Done: libpng sources copied"
else
  echo "Warning: fuzzbench-temp/benchmarks/libpng_libpng_read_fuzzer not found; skipping source copy" >&2
fi

# Search the repository to build a corpus
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_libpng_corpus.sh" ]; then
    "$SCRIPT_DIR/create_libpng_corpus.sh" || true
else
    echo "Warning: Executable create_libpng_corpus.sh not found, skipping corpus generation" >&2
fi
