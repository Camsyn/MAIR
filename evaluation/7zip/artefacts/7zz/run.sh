#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)

# Default: pass input via a file
export stdin="true"

# If input is provided, choose compression/decompression by extension and keep output under /tmp
if [ $# -ge 2 ]; then
  input="$2"
  TMP_DIR=$(mktemp -d /tmp/7zz_bench_XXXXXX)
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

$PROG_DIR/../../../run.sh $@
