#!/bin/bash
# Record the average real time of the target command only.
# Usage: collect-time.sh --label NAME [--repeat N] --out DIR -- command [args...]
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --label NAME [--repeat N] --out DIR -- command [args...]" >&2
  exit 2
}
parse_args "$@"
[[ -n "$LABEL" && -n "$OUT" && ${#COMMAND[@]} -gt 0 ]] || usage
[[ "$REPEATS" =~ ^[1-9][0-9]*$ ]] || { echo "repeat must be a positive integer" >&2; exit 1; }
log=$OUT/time-${LABEL//\//-}.log
mkdir -p "$OUT"
sum=0
stamp=$(mktemp)
for ((i = 1; i <= REPEATS; i++)); do
  TIMEFORMAT='%3R'
  if ! { time "${COMMAND[@]}" >"$log" 2>>"$log"; } 2>"$stamp"; then
    echo "workload failed; log: $log" >&2
    tail -n 40 "$log" >&2 || true
    exit 1
  fi
  elapsed=$(cat "$stamp")
  sum=$(awk -v s="$sum" -v e="$elapsed" 'BEGIN { printf "%.3f", s + e }')
done
average=$(awk -v s="$sum" -v n="$REPEATS" 'BEGIN { printf "%.3f", s / n }')
file=$OUT/time.tsv
begin_table "$file" $'label\taverage_seconds\trepeats'
append_row "$file" "$LABEL" "$average" "$REPEATS" >> "$file"
