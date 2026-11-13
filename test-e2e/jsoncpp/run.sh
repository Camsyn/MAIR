#!/bin/bash
# One-click jsoncpp end-to-end check for baseline and MAIR, under ASan and TSan.
#
# evaluation/jsoncpp/fetch.sh pins
# https://github.com/open-source-parsers/jsoncpp at
# 8190e061bc2d95da37479a638aa2c9e483e58ec6. Its corpus is every *.json in that
# tree larger than 512 bytes, except pass1.json and fail_test_stack_limit.json.
# evaluation/jsoncpp/compile.sh builds the static library and
# src/test_lib_json/fuzz.cpp with xclang++. ASan uses -asan and TSan uses
# -tsan, the project flags for evaluation's asan-new and tsan-new modes.
#
# This script does not execute evaluation/jsoncpp/compile.sh. That script
# deletes its build directory, so it cannot keep baseline/MAIR and ASan/TSan
# binaries together. It also leaves BUILD_OBJECT_LIBS on, which compiles the
# library sources a second time. This build keeps one static library.
#
# JSONCPP_E2E_REBUILD=1 compiles again. RUNS sets the libFuzzer budget.
# Each timed run gets its own copy of the seed corpus. libFuzzer writes new
# inputs back into the corpus directory, so a shared directory would make later
# variants and repeats run a different workload.
set -euo pipefail
# Non-interactive bash sources BASH_ENV. On this machine that loads nvm, and
# nvm calls `node`, which is itself a bash shim. Under perf's redirected
# stdout that recursion does not stop. Metrics must not inherit it.
unset BASH_ENV
unset ENV

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_DIR=${JSONCPP_E2E_SOURCE:-$SCRIPT_DIR/source}
JSONCPP_SRC=$SOURCE_DIR/jsoncpp
JSONCPP_COMMIT=8190e061bc2d95da37479a638aa2c9e483e58ec6

# Ambient CC/CXX often point at system Clang, which does not contain MAIR.
CC=${JSONCPP_E2E_CC:-$REPO_DIR/build/xclang}
CXX=${JSONCPP_E2E_CXX:-$REPO_DIR/build/xclang++}
LLVM_BIN=${LLVM_BIN:-/home/camsyn/kits/llvm/23/bin}
export LLVM_BIN
JOBS=${JOBS:-$(nproc)}
RUNS=${RUNS:-1000}
MAX_LEN=${JSONCPP_E2E_MAX_LEN:-65536}
REBUILD=${JSONCPP_E2E_REBUILD:-0}
STATS_ROOT=$SCRIPT_DIR/build/mair-stats

export GIT_TERMINAL_PROMPT=0
export CCACHE_DISABLE=1

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

fetch_sources() {
  local marker=src/test_lib_json/fuzz.cpp
  if [[ -f "$JSONCPP_SRC/$marker" ]]; then
    local have
    have=$(git -C "$JSONCPP_SRC" rev-parse HEAD)
    if [[ "$have" != "$JSONCPP_COMMIT" ]]; then
      echo "jsoncpp checkout $have is not $JSONCPP_COMMIT" >&2
      exit 1
    fi
    return 0
  fi
  if [[ -e "$JSONCPP_SRC" ]]; then
    echo "incomplete checkout: $JSONCPP_SRC" >&2
    exit 1
  fi
  mkdir -p "$SOURCE_DIR"
  echo "clone jsoncpp -> $JSONCPP_SRC"
  git clone https://github.com/open-source-parsers/jsoncpp "$JSONCPP_SRC"
  git -C "$JSONCPP_SRC" fetch --depth 1 origin "$JSONCPP_COMMIT"
  git -C "$JSONCPP_SRC" checkout --detach "$JSONCPP_COMMIT"
}

install_compiler_wrapper() {
  local wrap_dir=$SCRIPT_DIR/build/cc
  mkdir -p "$wrap_dir"
  cat > "$wrap_dir/xclang" <<EOF
#!/bin/bash
# Keep -mair-stats summaries for MAIR sanitizer compiles.
unset BASH_ENV
unset ENV
set -u
here=\$(cd -- "\$(dirname -- "\$0")" && pwd)
name=\$(basename -- "\$0")
if [[ "\$name" == *++* ]]; then
  real=$CXX
else
  real=$CC
fi
san=none
for arg in "\$@"; do
  case "\$arg" in
    -asan | -fsanitize=address) san=asan ;;
    -tsan | -fsanitize=thread) san=tsan ;;
  esac
done
extra=()
variant=baseline
if [[ -n "\${MAIR:-}" ]]; then
  variant=mair
  if [[ "\$san" != none ]]; then
    extra=(-mair-stats)
  fi
fi
stamp=\$(date +%s%N)
err=\$here/cc-err/\${san}-\${variant}-\${stamp}-\$\$.log
mkdir -p "\$here/cc-err" "$STATS_ROOT/\$san/\$variant"
"\$real" "\$@" "\${extra[@]}" 2>"\$err"
status=\$?
cat "\$err" >&2
if [[ "\$san" != none && "\$variant" == mair ]]; then
  awk '
    \$0 == "MAIR stats" { capture = 1; buf = \$0; next }
    capture && (\$0 ~ /^ / || \$0 == "") { buf = buf "\\n" \$0; next }
    capture { print buf; capture = 0 }
    END { if (capture) print buf }
  ' "\$err" > "$STATS_ROOT/\$san/\$variant/\${stamp}-\$\$.log"
fi
exit "\$status"
EOF
  chmod +x "$wrap_dir/xclang"
  ln -sfn xclang "$wrap_dir/xclang++"
  WRAP_CC=$wrap_dir/xclang
  WRAP_CXX=$wrap_dir/xclang++
}

retire_stats() {
  local sanitizer=$1
  local variant=$2
  local raw=$STATS_ROOT/$sanitizer/$variant
  if [[ ! -d "$raw" ]]; then
    return 0
  fi
  local dest=$REPO_DIR/.temp/recyclebin/$(date +%Y%m%d_%H%M%S)/jsoncpp-mair-stats/$sanitizer-$variant
  mkdir -p "$(dirname "$dest")"
  mv "$raw" "$dest"
}

build_one() {
  local sanitizer=$1
  local variant=$2
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  local oracle=$build_dir/jsoncpp_oracle
  local fuzzer=$build_dir/jsoncpp_fuzzer
  if [[ "$REBUILD" != 1 && -x "$oracle" && -x "$fuzzer" ]]; then
    echo "reuse $sanitizer $variant"
    return 0
  fi
  if [[ "$REBUILD" == 1 && -d "$build_dir" ]]; then
    local retired=$REPO_DIR/.temp/recyclebin/$(date +%Y%m%d_%H%M)/jsoncpp-build/$(date +%s%N)/$sanitizer/$variant
    mkdir -p "$(dirname "$retired")"
    mv "$build_dir" "$retired"
  fi
  mkdir -p "$build_dir"
  if [[ "$variant" == mair ]]; then
    export MAIR=1
    retire_stats "$sanitizer" "$variant"
  else
    unset MAIR
  fi
  local flag="-$sanitizer"
  echo "compile $sanitizer $variant"
  cmake -S "$SCRIPT_DIR" -B "$build_dir" -G Ninja \
    -DJSONCPP_SOURCE_DIR="$JSONCPP_SRC" \
    -DFUZZER_RUNTIME="$FUZZER_RT" \
    -DCMAKE_C_COMPILER="$WRAP_CC" \
    -DCMAKE_CXX_COMPILER="$WRAP_CXX" \
    -DCMAKE_BUILD_TYPE= \
    -DCMAKE_C_COMPILER_LAUNCHER= \
    -DCMAKE_CXX_COMPILER_LAUNCHER= \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$build_dir" \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$build_dir/lib" \
    -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$build_dir/lib" \
    -DCMAKE_C_FLAGS="-O2 -g $flag" \
    -DCMAKE_CXX_FLAGS="-O2 -g -stdlib=libc++ $flag -fsanitize=fuzzer-no-link" \
    -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ $flag"
  ninja -C "$build_dir" -j "$JOBS" jsoncpp_fuzzer jsoncpp_oracle
}

require_runtime() {
  local bin=$1
  local needle=$2
  if ! nm -C "$bin" | grep "$needle" >/dev/null; then
    echo "$bin does not reference $needle" >&2
    exit 1
  fi
}

prepare_seeds() {
  local seeds=$SCRIPT_DIR/build/seeds
  mkdir -p "$seeds"
  if [[ -e "$seeds/.ready" ]]; then
    printf '%s\n' "$seeds"
    return 0
  fi
  local count=0
  while IFS= read -r file; do
    local base
    base=$(basename -- "$file")
    case "$base" in
      pass1.json | fail_test_stack_limit.json) continue ;;
    esac
    cp "$file" "$seeds/$(printf '%03d' "$count")-$base"
    count=$((count + 1))
  done < <(find "$JSONCPP_SRC" -type f -name '*.json' -size +512c | sort)
  if [[ "$count" -eq 0 ]]; then
    echo "no jsoncpp seeds larger than 512 bytes" >&2
    exit 1
  fi
  : > "$seeds/.ready"
  echo "jsoncpp seeds: $count" >&2
  printf '%s\n' "$seeds"
}

install_driver() {
  local driver=$SCRIPT_DIR/build/drive-fuzzer.sh
  cat > "$driver" <<'EOF'
#!/bin/bash
unset BASH_ENV
unset ENV
set -euo pipefail
fuzzer=$1
seeds=$2
runs=$3
max_len=$4
root=$5
mkdir -p "$root"
dest=$root/$(date +%s%N)-$$
mkdir -p "$dest"
cp -a "$seeds/." "$dest/"
cd "$dest"
exec "$fuzzer" -runs="$runs" -seed=1 -max_len="$max_len" "$dest"
EOF
  chmod +x "$driver"
  printf '%s\n' "$driver"
}

run_one() (
  local sanitizer=$1
  local variant=$2
  local seeds=$3
  local driver=$4
  local build_dir=$SCRIPT_DIR/build/$sanitizer/$variant
  local oracle=$build_dir/jsoncpp_oracle
  local fuzzer=$build_dir/jsoncpp_fuzzer
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
  local runs_root=$SCRIPT_DIR/build/corpus-runs
  echo "== $sanitizer $variant oracle =="
  "$oracle"
  echo "collect $sanitizer/$variant"
  "$metrics/collect-size.sh" --label "$sanitizer/$variant" --binary "$fuzzer" --out "$metrics_out"
  "$metrics/collect-instrumentation.sh" --label "$sanitizer/$variant" --objects "$build_dir" --out "$metrics_out"
  "$metrics/collect-time.sh" --label "$sanitizer/$variant" --repeat "${METRICS_REPEAT:-5}" --out "$metrics_out" \
    -- "$driver" "$fuzzer" "$seeds" "$RUNS" "$MAX_LEN" "$runs_root"
  "$metrics/collect-perf.sh" --label "$sanitizer/$variant" --out "$metrics_out" \
    -- "$driver" "$fuzzer" "$seeds" "$RUNS" "$MAX_LEN" "$runs_root"
  echo "jsoncpp e2e ok: $fuzzer"
)

print_mair_stats() {
  local stats=$SCRIPT_DIR/build/metrics/mair-stats.txt
  {
    echo "== mair-stats =="
    echo "Sums of -mair-stats blocks from MAIR sanitizer compiles. in counts visits; compare the action fields."
    for sanitizer in asan tsan; do
      local raw=$STATS_ROOT/$sanitizer/mair
      echo "-- $sanitizer/mair --"
      if [[ ! -d "$raw" ]]; then
        echo "no records"
        continue
      fi
      awk '
        /^  pipeline / { pipelines[$2]++ }
        /^  functions / { functions += $2 }
        /^    / {
          pass = $1
          passes[pass] = 1
          for (i = 2; i < NF; i += 2)
            sum[pass, $i] += $(i + 1)
        }
        END {
          printf "functions %d\n", functions + 0
          npipe = 0
          for (name in pipelines)
            npipe++
          if (npipe == 0)
            print "pipeline none"
          for (name in pipelines)
            printf "pipeline %s x%d\n", name, pipelines[name]
          count = 0
          for (pass in passes)
            order[count++] = pass
          for (i = 0; i < count; i++) {
            for (j = i + 1; j < count; j++) {
              if (order[j] < order[i]) {
                tmp = order[i]
                order[i] = order[j]
                order[j] = tmp
              }
            }
          }
          split("in preheader exit range periodic merged intra inter eliminated hoisted sunk", fields, " ")
          for (i = 0; i < count; i++) {
            pass = order[i]
            printf "  %s", pass
            for (f = 1; f <= 11; f++) {
              key = pass SUBSEP fields[f]
              if (key in sum)
                printf "  %s %d", fields[f], sum[key]
            }
            printf "\n"
          }
        }
      ' "$raw"/*.log
    done
  } | tee "$stats"
}

fetch_sources
install_compiler_wrapper
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    build_one "$sanitizer" "$variant"
  done
done
seeds=$(prepare_seeds)
driver=$(install_driver)
mkdir -p "$SCRIPT_DIR/build/metrics"
for kind in size time instrumentation perf; do
  : > "$SCRIPT_DIR/build/metrics/$kind.tsv"
done
for sanitizer in asan tsan; do
  for variant in baseline mair; do
    run_one "$sanitizer" "$variant" "$seeds" "$driver"
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
echo
print_mair_stats
