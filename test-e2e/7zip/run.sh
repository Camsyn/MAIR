#!/bin/bash
# 7zz ASan/TSan x baseline/MAIR with a checked compression round trip.
set -euo pipefail
unset BASH_ENV ENV

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_DIR=${SEVENZIP_E2E_SOURCE:-$SCRIPT_DIR/source}
COMMIT=bd76c3ce3f8a4804274080e840d6a062454a7c73
CC=${SEVENZIP_E2E_CC:-$REPO_DIR/build/xclang}
CXX=${SEVENZIP_E2E_CXX:-$REPO_DIR/build/xclang++}
LLVM_BIN=${LLVM_BIN:-/home/camsyn/kits/llvm/23/bin}
export LLVM_BIN
export CCACHE_DISABLE=1
JOBS=${JOBS:-$(nproc)}
REBUILD=${SEVENZIP_E2E_REBUILD:-0}
TMPDIR=$REPO_DIR/.temp/null/$(date +%Y%m%d_%H%M)/7zip-$(date +%s%N)
mkdir -p "$TMPDIR"
export TMPDIR

if [[ ! -x "$CC" || ! -x "$CXX" || ! -x "$LLVM_BIN/clang" ]]; then
  echo "build xclang/xclang++ first and set LLVM_BIN to the LLVM 23 bin directory" >&2
  exit 1
fi

fetch_sources() {
  if [[ ! -d "$SOURCE_DIR" ]]; then
    git init -q "$SOURCE_DIR"
    git -C "$SOURCE_DIR" remote add origin https://github.com/mcmilk/7-Zip.git
    git -C "$SOURCE_DIR" fetch --depth 1 origin "$COMMIT"
    git -C "$SOURCE_DIR" checkout --detach FETCH_HEAD
  fi
  if [[ "$(git -C "$SOURCE_DIR" rev-parse HEAD)" != "$COMMIT" ]]; then
    echo "7zip source must be at $COMMIT: $SOURCE_DIR" >&2
    exit 1
  fi
  git -C "$SOURCE_DIR" diff --exit-code HEAD --
}

build_one() (
  local sanitizer=$1 variant=$2
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  if [[ "$REBUILD" != 1 && -x "$build_dir/7zz" ]]; then
    echo "reuse $sanitizer $variant"
    return 0
  fi
  if [[ "$REBUILD" == 1 && -d "$build_dir" ]]; then
    local retired=$REPO_DIR/.temp/recyclebin/$(date +%Y%m%d_%H%M)/7zip-build/$(date +%s%N)/$sanitizer/$variant
    mkdir -p "$(dirname "$retired")"
    mv "$build_dir" "$retired"
  fi
  mkdir -p "$build_dir"
  local flag="-$sanitizer" stats=
  if [[ "$variant" == mair ]]; then
    export MAIR=1
    stats=-mair-stats
  else
    unset MAIR
  fi
  echo "compile $sanitizer $variant"
  make -C "$SOURCE_DIR/CPP/7zip/Bundles/Alone2" -f makefile.gcc -j "$JOBS" all \
    O="$build_dir" CC="$CC" CXX="$CXX" DEBUG_BUILD=1 \
    CFLAGS_BASE2="$flag $stats" CXXFLAGS_BASE2="-stdlib=libc++ $flag $stats" \
    LDFLAGS="-stdlib=libc++ $flag" LFLAGS_NOEXECSTACK="-z noexecstack" \
    2>&1 | tee "$build_dir/build.log"
)

run_one() (
  local sanitizer=$1 variant=$2
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  local binary=$build_dir/7zz
  local asan_options=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:symbolize=1}
  local tsan_options=${TSAN_OPTIONS:-halt_on_error=1:symbolize=1}
  local asan_symbolizer=${ASAN_SYMBOLIZER_PATH:-$LLVM_BIN/llvm-symbolizer}
  local tsan_symbolizer=${TSAN_SYMBOLIZER_PATH:-$LLVM_BIN/llvm-symbolizer}
  unset ASAN_OPTIONS ASAN_SYMBOLIZER_PATH TSAN_OPTIONS TSAN_SYMBOLIZER_PATH
  case "$sanitizer" in
    asan) export ASAN_OPTIONS=$asan_options ASAN_SYMBOLIZER_PATH=$asan_symbolizer ;;
    tsan) export TSAN_OPTIONS=$tsan_options TSAN_SYMBOLIZER_PATH=$tsan_symbolizer ;;
  esac
  if ! nm -C "$binary" | grep "__${sanitizer}_" >/dev/null; then
    echo "sanitizer runtime missing: $binary" >&2
    exit 1
  fi
  local metrics=$REPO_DIR/test-e2e/metrics out=$SCRIPT_DIR/build/metrics
  local runs=$TMPDIR/runs/$sanitizer/$variant
  echo "== $sanitizer $variant round trip =="
  python3 "$SCRIPT_DIR/workload.py" "$binary" "$runs"
  "$metrics/collect-size.sh" --label "$sanitizer/$variant" --binary "$binary" --out "$out"
  "$metrics/collect-instrumentation.sh" --label "$sanitizer/$variant" --objects "$build_dir" --out "$out"
  "$metrics/collect-time.sh" --label "$sanitizer/$variant" --repeat "${METRICS_REPEAT:-5}" --out "$out" \
    -- python3 "$SCRIPT_DIR/workload.py" "$binary" "$runs"
  "$metrics/collect-perf.sh" --label "$sanitizer/$variant" --out "$out" \
    -- python3 "$SCRIPT_DIR/workload.py" "$binary" "$runs"
  echo "7zip e2e ok: $binary"
)

fetch_sources
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    build_one "$sanitizer" "$variant"
  done
done
mkdir -p "$SCRIPT_DIR/build/metrics"
for kind in size time instrumentation perf; do
  : > "$SCRIPT_DIR/build/metrics/$kind.tsv"
done
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    run_one "$sanitizer" "$variant"
  done
done
for kind in size time instrumentation perf; do
  echo
  "$REPO_DIR/test-e2e/metrics/print-$kind.sh" --out "$SCRIPT_DIR/build/metrics"
done
