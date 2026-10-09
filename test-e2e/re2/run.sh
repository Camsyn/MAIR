#!/bin/bash
# Build and run the RE2 ASan fuzzer with and without MAIR.
# This script does not delete directories and does not call evaluation/re2/compile.sh.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_ROOT=${RE2_E2E_SOURCE:-}
if [[ -z "$SOURCE_ROOT" ]]; then
  for candidate in \
      "$SCRIPT_DIR/source" \
      "$REPO_DIR/.temp/null/20261008_1752/re2_e2e/source"
  do
    if [[ -f "$candidate/re2/re2/fuzzing/re2_fuzzer.cc" && -f "$candidate/abseil-cpp/CMakeLists.txt" ]]; then
      SOURCE_ROOT=$candidate
      break
    fi
  done
fi
if [[ -z "$SOURCE_ROOT" ]]; then
  echo "RE2 and Abseil sources not found. Set RE2_E2E_SOURCE to a directory containing re2/ and abseil-cpp/." >&2
  exit 1
fi

CXX=${CXX:-$REPO_DIR/build/xclang++}
CC=${CC:-$REPO_DIR/build/xclang}
LLVM_BIN=${LLVM_BIN:-/home/camsyn/kits/llvm/23/bin}
FUZZER_RT=$("$LLVM_BIN/clang" -print-file-name=libclang_rt.fuzzer.a)
JOBS=${JOBS:-$(nproc)}
RUNS=${RUNS:-1000}

build_one() {
  local mode=$1
  local build_dir=$SCRIPT_DIR/build/$mode
  mkdir -p "$build_dir"
  if [[ "$mode" == "mair" ]]; then
    export MAIR=1
  else
    unset MAIR || true
  fi
  cmake -S "$SCRIPT_DIR" -B "$build_dir" -G Ninja \
    -DRE2_SOURCE_DIR="$SOURCE_ROOT/re2" \
    -DABSL_SOURCE_DIR="$SOURCE_ROOT/abseil-cpp" \
    -DFUZZER_RUNTIME="$FUZZER_RT" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_BUILD_TYPE= \
    -DCMAKE_CXX_FLAGS="-O2 -g -stdlib=libc++ -asan -fsanitize=fuzzer-no-link" \
    -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++"
  ninja -C "$build_dir" -j "$JOBS" re2_fuzzer re2_oracle
}

build_one baseline
build_one mair

CORPUS=$SCRIPT_DIR/build/corpus
mkdir -p "$CORPUS"
python3 -c 'import pathlib,sys; corpus=pathlib.Path(sys.argv[1]); samples=[b"abc", b"a.c", b"(cat|dog)+", b"[", b"", b"123"];
[ (corpus / ("sample-%02d" % i)).write_bytes(sample) for i, sample in enumerate(samples) ]' "$CORPUS"

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:symbolize=1}
export ASAN_SYMBOLIZER_PATH=${ASAN_SYMBOLIZER_PATH:-$LLVM_BIN/llvm-symbolizer}
for mode in baseline mair; do
  echo "== $mode oracle =="
  "$SCRIPT_DIR/build/$mode/re2_oracle"
  echo "== $mode fuzzer =="
  "$SCRIPT_DIR/build/$mode/re2_fuzzer" -runs="$RUNS" -seed=1 -max_len=256 "$CORPUS"
done
echo "re2 e2e ok: $SCRIPT_DIR/build/baseline/re2_fuzzer"
echo "re2 e2e ok: $SCRIPT_DIR/build/mair/re2_fuzzer"
