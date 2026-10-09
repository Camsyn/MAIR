# Shared argument and table helpers for the metric scripts in this directory.
# Each metric has its own collect script and print script.

usage() {
  echo "Usage: $0" >&2
  exit 2
}

require_value() {
  [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || usage
  printf '%s' "$2"
}

begin_table() {
  local file=$1
  local header=$2
  mkdir -p "$(dirname "$file")"
  if [[ ! -s "$file" ]]; then
    printf '%s\n' "$header" > "$file"
  fi
}

append_row() {
  local file=$1
  shift
  local first=1
  local field
  for field in "$@"; do
    field=${field//$'\t'/ }
    if [[ $first -eq 1 ]]; then
      printf '%s' "$field"
      first=0
    else
      printf '\t%s' "$field"
    fi
  done
  printf '\n'
}

parse_args() {
  LABEL=
  OUT=
  BINARY=
  OBJECTS=
  REPEATS=${METRICS_REPEAT:-5}
  COMMAND=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --label) LABEL=$(require_value "$@"); shift 2 ;;
      --out) OUT=$(require_value "$@"); shift 2 ;;
      --binary) BINARY=$(require_value "$@"); shift 2 ;;
      --objects) OBJECTS=$(require_value "$@"); shift 2 ;;
      --repeat) REPEATS=$(require_value "$@"); shift 2 ;;
      --) shift; COMMAND=("$@"); break ;;
      *) usage ;;
    esac
  done
}

print_records() {
  local title=$1
  local file=$2
  local note=$3
  local fmt=$4
  echo "== $title =="
  if [[ -n "$note" ]]; then
    echo "$note"
  fi
  if [[ ! -s "$file" ]]; then
    echo "no records"
    return 0
  fi
  awk -F '\t' -v fmt="$fmt" '
    FNR == 1 { next }
    {
      for (i = 1; i <= NF; i++) if ($i == "") $i = "unsupported"
      printf fmt "\n", $1, $2, $3, $4, $5, $6, $7
    }
  ' "$file"
}
