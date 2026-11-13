#!/bin/bash
# SQLite ASan/TSan x baseline/MAIR, using the official 3.33.0 release.
# See README.md for prerequisites, inputs, metrics and rebuild behavior.
set -euo pipefail
unset BASH_ENV ENV

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_DIR=${SQLITE3_E2E_SOURCE:-$SCRIPT_DIR/source}
SQLITE_SRC=$SOURCE_DIR/sqlite-amalgamation-3330000
DRIVER_SRC=$SOURCE_DIR/sqlite-src-3330000
SQLITE_COMMIT=fca8dc8b578f215a969cd899336378966156154710873e68b3d9ac5881b0ff3f
CC=${SQLITE3_E2E_CC:-$REPO_DIR/build/xclang}
CXX=${SQLITE3_E2E_CXX:-$REPO_DIR/build/xclang++}
LLVM_BIN=${LLVM_BIN:-/home/camsyn/kits/llvm/23/bin}
export LLVM_BIN
export CCACHE_DISABLE=1
JOBS=${JOBS:-$(nproc)}
RUNS=${RUNS:-1000}
MAX_LEN=${SQLITE3_E2E_MAX_LEN:-65536}
REBUILD=${SQLITE3_E2E_REBUILD:-0}

if [[ ! -x "$CC" || ! -x "$CXX" || ! -x "$LLVM_BIN/clang" ]]; then
  echo "build xclang/xclang++ first and set LLVM_BIN to the LLVM 23 bin directory" >&2
  exit 1
fi
FUZZER_RT=$("$LLVM_BIN/clang" -print-file-name=libclang_rt.fuzzer.a)
if [[ ! -f "$FUZZER_RT" ]]; then
  echo "libFuzzer runtime not found: $FUZZER_RT" >&2
  exit 1
fi

fetch_archive() {
  local name=$1
  local sha=$2
  local archive=$SOURCE_DIR/$name.zip
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 --connect-timeout 20 \
      "https://sqlite.org/2020/$name.zip" --output "$archive"
  fi
  printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
  # Keep extraction deterministic on repeated runs without overwriting files.
  if [[ ! -d "$SOURCE_DIR/$name" ]]; then
    unzip -q "$archive" -d "$SOURCE_DIR"
  fi
}

fetch_sources() {
  mkdir -p "$SOURCE_DIR"
  fetch_archive sqlite-amalgamation-3330000 b34f4c0c0eefad9a7e515c030c18702e477f4ef7d8ade6142bdab8011b487ac6
  fetch_archive sqlite-src-3330000 90bf7604a5aa26deece551af7a665fd4ce3d854ea809899c0e4bb19a69d609b8
  if [[ "$(cat "$DRIVER_SRC/manifest.uuid")" != "$SQLITE_COMMIT" ]] ||
     ! grep -F "#define SQLITE_SOURCE_ID      \"2020-08-14 13:23:32 $SQLITE_COMMIT\"" "$SQLITE_SRC/sqlite3.h" >/dev/null; then
    echo "SQLite source and amalgamation must both match $SQLITE_COMMIT" >&2
    exit 1
  fi
}

build_one() {
  local sanitizer=$1 variant=$2
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  if [[ "$REBUILD" != 1 && -x "$build_dir/sqlite3_oracle" && -x "$build_dir/sqlite3_fuzzer" ]]; then
    echo "reuse $sanitizer $variant"
    return 0
  fi
  if [[ "$REBUILD" == 1 && -d "$build_dir" ]]; then
    local retired=$REPO_DIR/.temp/recyclebin/$(date +%Y%m%d_%H%M)/sqlite3-build/$(date +%s%N)/$sanitizer/$variant
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
  cmake -S "$SCRIPT_DIR" -B "$build_dir" -G Ninja \
    -DSQLITE3_SOURCE_DIR="$SQLITE_SRC" \
    -DSQLITE3_DRIVER="$DRIVER_SRC/test/ossfuzz.c" \
    -DFUZZER_RUNTIME="$FUZZER_RT" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= \
    -DCMAKE_BUILD_TYPE= \
    -DCMAKE_C_FLAGS="-O2 -g $flag $stats -fsanitize=fuzzer-no-link" \
    -DCMAKE_CXX_FLAGS="-O2 -g -stdlib=libc++ $flag $stats -fsanitize=fuzzer-no-link" \
    -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ $flag"
  ninja -C "$build_dir" -j "$JOBS" sqlite3_fuzzer sqlite3_oracle 2>&1 | tee "$build_dir/build.log"
}

install_driver() {
  local driver=$SCRIPT_DIR/build/drive-fuzzer.sh
  cat > "$driver" <<'EOF'
#!/bin/bash
unset BASH_ENV ENV
set -euo pipefail
fuzzer=$1 seeds=$2 runs=$3 max_len=$4 root=$5
dest=$root/$(date +%s%N)-$$
mkdir -p "$dest"
cp "$seeds"/*.sql "$dest/"
cd "$dest"
exec "$fuzzer" -runs="$runs" -seed=1 -max_len="$max_len" "$dest"
EOF
  chmod +x "$driver"
  printf '%s\n' "$driver"
}

run_one() (
  local sanitizer=$1 variant=$2 driver=$3
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  local asan_options=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:symbolize=1}
  local tsan_options=${TSAN_OPTIONS:-halt_on_error=1:symbolize=1}
  local asan_symbolizer=${ASAN_SYMBOLIZER_PATH:-$LLVM_BIN/llvm-symbolizer}
  local tsan_symbolizer=${TSAN_SYMBOLIZER_PATH:-$LLVM_BIN/llvm-symbolizer}
  unset ASAN_OPTIONS ASAN_SYMBOLIZER_PATH TSAN_OPTIONS TSAN_SYMBOLIZER_PATH
  case "$sanitizer" in
    asan) export ASAN_OPTIONS=$asan_options ASAN_SYMBOLIZER_PATH=$asan_symbolizer ;;
    tsan) export TSAN_OPTIONS=$tsan_options TSAN_SYMBOLIZER_PATH=$tsan_symbolizer ;;
  esac
  local binary
  for binary in "$build_dir/sqlite3_oracle" "$build_dir/sqlite3_fuzzer"; do
    if ! nm -C "$binary" | grep "__${sanitizer}_" >/dev/null; then
      echo "sanitizer runtime missing: $binary" >&2
      exit 1
    fi
  done
  echo "== $sanitizer $variant oracle =="
  "$build_dir/sqlite3_oracle"
  local metrics=$REPO_DIR/test-e2e/metrics out=$SCRIPT_DIR/build/metrics
  "$metrics/collect-size.sh" --label "$sanitizer/$variant" --binary "$build_dir/sqlite3_fuzzer" --out "$out"
  "$metrics/collect-instrumentation.sh" --label "$sanitizer/$variant" --objects "$build_dir" --out "$out"
  "$metrics/collect-time.sh" --label "$sanitizer/$variant" --repeat "${METRICS_REPEAT:-5}" --out "$out" \
    -- "$driver" "$build_dir/sqlite3_fuzzer" "$SCRIPT_DIR/seeds" "$RUNS" "$MAX_LEN" "$SCRIPT_DIR/build/corpus-runs"
  "$metrics/collect-perf.sh" --label "$sanitizer/$variant" --out "$out" \
    -- "$driver" "$build_dir/sqlite3_fuzzer" "$SCRIPT_DIR/seeds" "$RUNS" "$MAX_LEN" "$SCRIPT_DIR/build/corpus-runs"
  echo "sqlite3 e2e ok: $build_dir/sqlite3_fuzzer"
)

fetch_sources
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    build_one "$sanitizer" "$variant"
  done
done
driver=$(install_driver)
mkdir -p "$SCRIPT_DIR/build/metrics"
for kind in size time instrumentation perf; do
  : > "$SCRIPT_DIR/build/metrics/$kind.tsv"
done
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    run_one "$sanitizer" "$variant" "$driver"
  done
done
for kind in size time instrumentation perf; do
  echo
  "$REPO_DIR/test-e2e/metrics/print-$kind.sh" --out "$SCRIPT_DIR/build/metrics"
done
