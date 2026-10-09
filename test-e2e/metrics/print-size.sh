#!/bin/bash
# Print binary size records.
# Usage: print-size.sh --out DIR
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --out DIR" >&2
  exit 2
}
parse_args "$@"
[[ -n "$OUT" ]] || usage
print_records "binary size" "$OUT/size.tsv" "" \
  '%s  binary_bytes=%s  text_bytes=%s'
