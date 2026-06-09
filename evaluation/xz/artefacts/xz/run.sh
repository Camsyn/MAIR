#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)

# Smoke test: run only basic operations by default to avoid expensive compression/decompression
: "${SMOKE:=1}"
export stdin="false"

if [ "$SMOKE" = "1" ] || [ "$SMOKE" = "true" ]; then
  # Only test basic help output, without doing any file I/O
  export PRE_OPT="--help"
  export POST_OPT=""
else
  # Normal mode: pass input through a file
  # If input is provided, choose compression/decompression by extension and keep output under /tmp
  if [ $# -ge 2 ]; then
    input="$2"
    TMP_DIR=$(mktemp -d /tmp/xz_bench_XXXXXX)
    # Decompression: write output into the temporary directory under /tmp
    if [[ "$input" == *.xz ]]; then
      export PRE_OPT="-d -c"
      export POST_OPT="> $TMP_DIR/output"
    else
      # Compression: write the resulting archive into the temporary directory
      export PRE_OPT="-c"
      export POST_OPT="> $TMP_DIR/output.xz"
    fi
  else
    export PRE_OPT=""
    export POST_OPT=""
  fi
fi

$PROG_DIR/../../../run.sh $@