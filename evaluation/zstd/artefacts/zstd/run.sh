#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)

# Smoke test: run only basic operations by default to avoid expensive compression/decompression
: "${SMOKE:=1}"
export stdin="false"

if [ "$SMOKE" = "1" ] || [ "$SMOKE" = "true" ]; then
  # Only test basic help output, without doing any file I/O
  export PRE_OPT=""
  export POST_OPT="--help"
else
  # Normal mode: if input is provided, choose compression/decompression by extension and keep output under /tmp
  if [ $# -ge 2 ]; then
    input="$2"
    TMP_DIR=$(mktemp -d /tmp/zstd_bench_XXXXXX)
    # Decompression: write output into the temporary directory under /tmp
    if [[ "$input" == *.zst || "$input" == *.zstd ]]; then
      export PRE_OPT="-d -o $TMP_DIR/output"
      export POST_OPT=""
    else
      # Compression: write the resulting archive into the temporary directory
      export PRE_OPT="-o $TMP_DIR/output.zst"
      export POST_OPT=""
    fi
  else
    export PRE_OPT=""
    export POST_OPT=""
  fi
fi

# Combined asan+msan mode: keep reports visible, make them non-fatal, and disable LSan to avoid MSan conflicts
# mode="$1"
# if [[ "$mode" == *asan* && "$mode" == *msan* ]]; then
#   : "${ASAN_OPTIONS:=halt_on_error=0,detect_leaks=0}"
#   : "${MSAN_OPTIONS:=halt_on_error=0}"
#   : "${XSAN_OPTIONS:=halt_on_error=0}"
#   export ASAN_OPTIONS MSAN_OPTIONS XSAN_OPTIONS
# fi

$PROG_DIR/../../../run.sh $@