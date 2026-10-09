#!/bin/bash
# Record perf counters for one command. Unavailable events are "unsupported".
# Usage: collect-perf.sh --label NAME --out DIR -- command [args...]
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --label NAME --out DIR -- command [args...]" >&2
  exit 2
}
parse_args "$@"
[[ -n "$LABEL" && -n "$OUT" && ${#COMMAND[@]} -gt 0 ]] || usage
instructions=unsupported
branches=unsupported
branch_misses=unsupported
cache_references=unsupported
cache_misses=unsupported
cache_hits=unsupported
if command -v perf >/dev/null 2>&1; then
  stat=$(mktemp)
  log=$OUT/perf-${LABEL//\//-}.log
  mkdir -p "$OUT"
  set +e
  perf stat -x, -e instructions,branches,branch-misses,cache-references,cache-misses -- \
    "${COMMAND[@]}" >"$log" 2>"$stat"
  status=$?
  set -e
  if [[ $status -eq 0 ]] || grep -q ',' "$stat"; then
    parsed=$(awk -F, '
      function clean(name) { sub(/:.*$/, "", name); return name }
      NF >= 3 {
        event = clean($3)
        value = $1
        if (value ~ /not supported/ || value ~ /not counted/ || value == "")
          value = "unsupported"
        if (event == "instructions" || event == "branches" || event == "branch-misses" ||
            event == "cache-references" || event == "cache-misses")
          printf "%s %s\n", event, value
      }
    ' "$stat")
    while read -r event value; do
      [[ -z "${event:-}" ]] && continue
      case "$event" in
        instructions) instructions=$value ;;
        branches) branches=$value ;;
        branch-misses) branch_misses=$value ;;
        cache-references) cache_references=$value ;;
        cache-misses) cache_misses=$value ;;
      esac
    done <<< "$parsed"
    if [[ "$cache_references" != unsupported && "$cache_misses" != unsupported ]]; then
      cache_hits=$(awk -v r="$cache_references" -v m="$cache_misses" 'BEGIN { printf "%.0f", r - m }')
    fi
  fi
fi
file=$OUT/perf.tsv
begin_table "$file" $'label\tinstructions\tbranches\tbranch_misses\tcache_references\tcache_misses\tcache_hits'
append_row "$file" "$LABEL" "$instructions" "$branches" "$branch_misses" \
  "$cache_references" "$cache_misses" "$cache_hits" >> "$file"
