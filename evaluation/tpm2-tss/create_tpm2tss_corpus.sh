#!/bin/bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
ARTEFACTS_DIR="$ROOT_DIR/artefacts"
CORPUS_ROOT="$ROOT_DIR/corpus"
mkdir -p "$CORPUS_ROOT"

generate_basic_corpus() {
  local target_dir="$1"
  mkdir -p "$target_dir"

  cat >"$target_dir/min.json" <<'JSON'
{}
JSON

  cat >"$target_dir/sample.json" <<'JSON'
{"a":1,"b":[true,false],"s":"x"}
JSON

  # Simple binary placeholder
  printf '\x00\x01\x02\x03' >"$target_dir/min.bin"
}

# Generate valid IMA event log corpus for fapi-ima-fuzzing (decoded from .b64)
generate_fapi_ima_corpus() {
  local target_dir="$1"
  mkdir -p "$target_dir"

  local src_dir="$ROOT_DIR/repo/test/data/fapi/eventlog"
  local files=(
    "sml-ima-ng-sha1.b64"
    "sml-ima-sha1.b64"
    "sml-ima-sig-sha256.b64"
  )

  local any=0
  for f in "${files[@]}"; do
    if [ -f "$src_dir/$f" ]; then
      local out="$target_dir/${f%.b64}"
      base64 -d "$src_dir/$f" >"$out" || { echo "Warning: failed to decode $f" >&2; rm -f "$out" || true; continue; }
      any=1
    fi
  done

  if [ "$any" -eq 0 ]; then
    echo "Warning: no IMA event log samples found in $src_dir, falling back to basic corpus." >&2
    generate_basic_corpus "$target_dir"
  fi
}

# Generate firmware event log corpus for fapi-system-fuzzing (decoded from .b64)
generate_fapi_system_corpus() {
  local target_dir="$1"
  mkdir -p "$target_dir"

  local src_dir="$ROOT_DIR/repo/test/data/fapi/eventlog"
  local files=(
    "binary_measurements_pc_client.b64"
    "binary_measurements_nuc.b64"
    "binary_measurements_hcrtm.b64"
    "event.b64"
    "event-uefivar.b64"
    "event-uefiaction.b64"
    "event-uefiservices.b64"
    "specid-vendordata.b64"
  )

  local any=0
  for f in "${files[@]}"; do
    if [ -f "$src_dir/$f" ]; then
      local out="$target_dir/${f%.b64}"
      base64 -d "$src_dir/$f" >"$out" || { echo "Warning: failed to decode $f" >&2; rm -f "$out" || true; continue; }
      any=1
    fi
  done

  if [ "$any" -eq 0 ]; then
    echo "Warning: no firmware event log samples found in $src_dir, falling back to basic corpus." >&2
    generate_basic_corpus "$target_dir"
  fi
}

# For each artefacts subdirectory, create a matching corpus subdirectory and place a sample there
shopt -s nullglob
created=()
for dir in "$ARTEFACTS_DIR"/*; do
  [ -d "$dir" ] || continue
  name="$(basename "$dir")"
  out_dir="$CORPUS_ROOT/$name"
  case "$name" in
    fapi-ima-fuzzing)
      generate_fapi_ima_corpus "$out_dir"
      ;;
    fapi-system-fuzzing)
      generate_fapi_system_corpus "$out_dir"
      ;;
    *)
      generate_basic_corpus "$out_dir"
      ;;
  esac
  created+=("$out_dir")
done

if [ ${#created[@]} -eq 0 ]; then
  echo "Warning: no subdirectories found under $ARTEFACTS_DIR, corpus not generated." >&2
else
  printf "Corpus directories generated:\n"
  for p in "${created[@]}"; do echo "  - $p"; done
fi



