#!/bin/bash
# Print perf records.
# Usage: print-perf.sh --out DIR
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --out DIR" >&2
  exit 2
}
parse_args "$@"
[[ -n "$OUT" ]] || usage
print_records "perf" "$OUT/perf.tsv" \
  "Hardware counters that this machine cannot read are unsupported." \
  '%s  instructions=%s  branches=%s  branch-misses=%s  cache-references=%s  cache-misses=%s  cache-hits=%s'
