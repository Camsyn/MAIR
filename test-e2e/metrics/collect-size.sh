#!/bin/bash
# Record binary file size and text-segment size.
# Usage: collect-size.sh --label NAME --binary PATH --out DIR
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --label NAME --binary PATH --out DIR" >&2
  exit 2
}
parse_args "$@"
[[ -n "$LABEL" && -n "$OUT" && -n "$BINARY" ]] || usage
[[ -f "$BINARY" ]] || { echo "binary not found: $BINARY" >&2; exit 1; }
bytes=$(stat -c %s "$BINARY")
if ! text=$(size "$BINARY" | awk 'NR == 2 { print $1 }') || [[ -z "$text" ]]; then
  text=unsupported
fi
file=$OUT/size.tsv
begin_table "$file" $'label\tbinary_bytes\ttext_bytes'
append_row "$file" "$LABEL" "$bytes" "$text" >> "$file"
