#!/bin/bash
# Print instrumented-MOP records.
# Usage: print-instrumentation.sh --out DIR
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --out DIR" >&2
  exit 2
}
parse_args "$@"
[[ -n "$OUT" ]] || usage
print_records "instrumented MOPs" "$OUT/instrumentation.tsv" \
  "Check sites in object files. One MA lowered to one check counts once." \
  '%s  instrumented_mops=%s'
