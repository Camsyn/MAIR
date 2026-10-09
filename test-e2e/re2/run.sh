#!/bin/bash
# One-click RE2 end-to-end check for baseline and MAIR, under ASan and TSan.
#
# The first run clones the same repositories as evaluation/re2 and compiles
# them. Later runs reuse test-e2e/re2/source and test-e2e/re2/build.
#
# evaluation/re2/fetch.sh clones https://github.com/google/re2.git --depth 1.
# evaluation/re2/compile.sh clones https://github.com/abseil/abseil-cpp.git
# --depth 1 and compiles with xclang/xclang++, -O2 -g, libc++, and the
# sanitizer flag. ASan uses -asan and TSan uses -tsan, which are the project
# compiler flags corresponding to evaluation's asan-new and tsan-new modes.
#
# This script does not execute evaluation/re2/compile.sh. That script deletes
# its build directory and cleans the RE2 source tree, so it cannot retain the
# baseline/MAIR and ASan/TSan binaries together.
#
# RE2_E2E_REBUILD=1 compiles again. RUNS sets the libFuzzer budget.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_DIR=${RE2_E2E_SOURCE:-$SCRIPT_DIR/source}
RE2_SRC=$SOURCE_DIR/re2
ABSL_SRC=$SOURCE_DIR/abseil-cpp

# Ambient CC/CXX often point at system Clang, which does not contain MAIR.
# evaluation/re2 also forces xclang/xclang++ rather than inheriting CC/CXX.
CC=${RE2_E2E_CC:-$REPO_DIR/build/xclang}
CXX=${RE2_E2E_CXX:-$REPO_DIR/build/xclang++}
LLVM_BIN=${LLVM_BIN:-/home/camsyn/kits/llvm/23/bin}
export LLVM_BIN
JOBS=${JOBS:-$(nproc)}
RUNS=${RUNS:-1000}
REBUILD=${RE2_E2E_REBUILD:-0}

export GIT_TERMINAL_PROMPT=0

if [[ ! -x "$CXX" || ! -x "$CC" ]]; then
  echo "xclang not found at $CC / $CXX. Build the MAIR compiler first." >&2
  exit 1
fi
if [[ ! -x "$LLVM_BIN/clang" ]]; then
  echo "clang not found at $LLVM_BIN/clang." >&2
  exit 1
fi

FUZZER_RT=$("$LLVM_BIN/clang" -print-file-name=libclang_rt.fuzzer.a)
if [[ ! -f "$FUZZER_RT" ]]; then
  echo "libFuzzer runtime not found: $FUZZER_RT" >&2
  exit 1
fi

clone_repo() {
  local url=$1
  local dest=$2
  local marker=$3
  if [[ -f "$dest/$marker" ]]; then
    return 0
  fi
  if [[ -e "$dest" ]]; then
    echo "incomplete checkout: $dest" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$dest")"
  echo "clone $url -> $dest"
  git clone --depth 1 "$url" "$dest"
}

fetch_sources() {
  clone_repo https://github.com/google/re2.git "$RE2_SRC" re2/fuzzing/re2_fuzzer.cc
  clone_repo https://github.com/abseil/abseil-cpp.git "$ABSL_SRC" CMakeLists.txt
}

build_one() {
  local sanitizer=$1
  local variant=$2
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  local oracle=$build_dir/re2_oracle
  local fuzzer=$build_dir/re2_fuzzer
  if [[ "$REBUILD" != 1 && -x "$oracle" && -x "$fuzzer" ]]; then
    echo "reuse $sanitizer $variant"
    return 0
  fi
  mkdir -p "$build_dir"
  if [[ "$variant" == mair ]]; then
    export MAIR=1
  else
    unset MAIR
  fi
  local flag="-$sanitizer"
  echo "compile $sanitizer $variant"
  cmake -S "$SCRIPT_DIR" -B "$build_dir" -G Ninja \
    -DRE2_SOURCE_DIR="$RE2_SRC" \
    -DABSL_SOURCE_DIR="$ABSL_SRC" \
    -DFUZZER_RUNTIME="$FUZZER_RT" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_BUILD_TYPE= \
    -DCMAKE_C_FLAGS="-O2 -g $flag" \
    -DCMAKE_CXX_FLAGS="-O2 -g -stdlib=libc++ $flag -fsanitize=fuzzer-no-link" \
    -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ $flag"
  ninja -C "$build_dir" -j "$JOBS" re2_fuzzer re2_oracle
}

require_runtime() {
  local bin=$1
  local needle=$2
  # grep -q would close the pipe early. With pipefail, nm then dies on
  # SIGPIPE and the check fails even though the symbol is present.
  if ! nm -C "$bin" | grep "$needle" >/dev/null; then
    echo "$bin does not reference $needle" >&2
    exit 1
  fi
}

prepare_corpus() {
  local corpus=$SCRIPT_DIR/build/corpus
  mkdir -p "$corpus"
  if [[ ! -e "$corpus/sample-00" ]]; then
    printf 'abc' > "$corpus/sample-00"
    printf 'a.c' > "$corpus/sample-01"
    printf '(cat|dog)+' > "$corpus/sample-02"
    printf '[' > "$corpus/sample-03"
    : > "$corpus/sample-04"
    printf '123' > "$corpus/sample-05"
  fi
  printf '%s\n' "$corpus"
}

run_one() {
  local sanitizer=$1
  local variant=$2
  local corpus=$3
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  local oracle=$build_dir/re2_oracle
  local fuzzer=$build_dir/re2_fuzzer
  local asan_options=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:symbolize=1}
  local asan_symbolizer=${ASAN_SYMBOLIZER_PATH:-$LLVM_BIN/llvm-symbolizer}
  local tsan_options=${TSAN_OPTIONS:-halt_on_error=1:symbolize=1}
  local tsan_symbolizer=${TSAN_SYMBOLIZER_PATH:-$LLVM_BIN/llvm-symbolizer}
  unset ASAN_OPTIONS ASAN_SYMBOLIZER_PATH TSAN_OPTIONS TSAN_SYMBOLIZER_PATH
  case "$sanitizer" in
    asan)
      require_runtime "$oracle" "__asan_"
      export ASAN_OPTIONS=$asan_options
      export ASAN_SYMBOLIZER_PATH=$asan_symbolizer
      ;;
    tsan)
      require_runtime "$oracle" "__tsan_"
      export TSAN_OPTIONS=$tsan_options
      export TSAN_SYMBOLIZER_PATH=$tsan_symbolizer
      ;;
    *)
      echo "unknown sanitizer: $sanitizer" >&2
      exit 1
      ;;
  esac
  local metrics=$REPO_DIR/test-e2e/metrics
  local metrics_out=$SCRIPT_DIR/build/metrics
  echo "== $sanitizer $variant oracle =="
  "$oracle"
  echo "collect $sanitizer/$variant"
  "$metrics/collect-size.sh" --label "$sanitizer/$variant" --binary "$fuzzer" --out "$metrics_out"
  "$metrics/collect-instrumentation.sh" --label "$sanitizer/$variant" --objects "$build_dir" --out "$metrics_out"
  "$metrics/collect-time.sh" --label "$sanitizer/$variant" --repeat "${METRICS_REPEAT:-5}" --out "$metrics_out" \
    -- "$fuzzer" -runs="$RUNS" -seed=1 -max_len=256 "$corpus"
  "$metrics/collect-perf.sh" --label "$sanitizer/$variant" --out "$metrics_out" \
    -- "$fuzzer" -runs="$RUNS" -seed=1 -max_len=256 "$corpus"
  echo "re2 e2e ok: $fuzzer"
}

fetch_sources
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    build_one "$sanitizer" "$variant"
  done
done
corpus=$(prepare_corpus)
mkdir -p "$SCRIPT_DIR/build/metrics"
for kind in size time instrumentation perf; do
  : > "$SCRIPT_DIR/build/metrics/$kind.tsv"
done
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    run_one "$sanitizer" "$variant" "$corpus"
  done
done
echo
"$REPO_DIR/test-e2e/metrics/print-size.sh" --out "$SCRIPT_DIR/build/metrics"
echo
"$REPO_DIR/test-e2e/metrics/print-time.sh" --out "$SCRIPT_DIR/build/metrics"
echo
"$REPO_DIR/test-e2e/metrics/print-instrumentation.sh" --out "$SCRIPT_DIR/build/metrics"
echo
"$REPO_DIR/test-e2e/metrics/print-perf.sh" --out "$SCRIPT_DIR/build/metrics"
