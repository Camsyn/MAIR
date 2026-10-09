#!/bin/bash
# Record sanitizer check sites in the object files of one build.
# Usage: collect-instrumentation.sh --label NAME --objects DIR --out DIR
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
usage() {
  echo "Usage: $0 --label NAME --objects DIR --out DIR" >&2
  exit 2
}

count_instrumented_mops() {
  local objects=$1
  local objdump=${LLVM_OBJDUMP:-}
  if [[ -z "$objdump" && -n "${LLVM_BIN:-}" ]]; then
    objdump=$LLVM_BIN/llvm-objdump
  fi
  if [[ -z "$objdump" ]]; then
    objdump=$(command -v llvm-objdump || true)
  fi
  if [[ -z "$objdump" || ! -x "$objdump" ]]; then
    echo "llvm-objdump not found" >&2
    printf 'unsupported'
    return 0
  fi
  if [[ ! -d "$objects" ]]; then
    echo "object directory not found: $objects" >&2
    printf 'unsupported'
    return 0
  fi
  local total=0 found=0 obj count
  while IFS= read -r obj; do
    found=1
    count=$("$objdump" -r "$obj" | awk '
      $2 ~ /^R_/ {
        name = $3
        sub(/[-+].*$/, "", name)
        if (name ~ /^__asan_report_(exp_)?(load|store)/) c++
        else if (name ~ /^__asan_(load|store)(1|2|4|8|16|N)(_noabort)?$/) c++
        else if (name ~ /^__asan_(memcpy|memset|memmove)$/) c++
        else if (name ~ /^__tsan_(unaligned_)?(read|write)/) c++
        else if (name ~ /^__tsan_(memcpy|memset|memmove)$/) c++
        else if (name ~ /^__tsan_atomic(8|16|32|64|128)_(load|store)/) c++
        else if (name ~ /^__tsan_vptr_(read|update)$/) c++
      }
      END { print c + 0 }
    ')
    total=$((total + count))
  done < <(find "$objects" -type f -name '*.o' \
    ! -path '*CompilerId*' \
    ! -path '*CMakeDetermineCompilerABI*' \
    ! -name 'CMakeCCompilerABI*.o' \
    ! -name 'CMakeCXXCompilerABI*.o')
  if [[ "$found" -eq 0 ]]; then
    printf 'unsupported'
    return 0
  fi
  printf '%s' "$total"
}

parse_args "$@"
[[ -n "$LABEL" && -n "$OUT" && -n "$OBJECTS" ]] || usage
count=$(count_instrumented_mops "$OBJECTS")
file=$OUT/instrumentation.tsv
begin_table "$file" $'label\tinstrumented_mops'
append_row "$file" "$LABEL" "$count" >> "$file"
