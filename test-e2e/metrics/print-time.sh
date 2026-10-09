#!/bin/bash
# Print execution-time records.
# Usage: print-time.sh --out DIR
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --out DIR" >&2
  exit 2
}
parse_args "$@"
[[ -n "$OUT" ]] || usage
print_records "execution time" "$OUT/time.tsv" \
  "Average real time of the target command only. Shell bookkeeping is not included." \
  '%s  average_seconds=%s  repeats=%s'
