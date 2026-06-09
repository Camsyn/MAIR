#!/bin/bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
ARTEFACTS_DIR="$ROOT_DIR/artefacts"
CORPUS_DIR="$ROOT_DIR/corpus"
REPO_DIR="$ROOT_DIR/repo"

mkdir -p "$CORPUS_DIR"

# For each artefacts/<name> generate a corresponding corpus/<name>
copy_from_tree() {
    local src_root="$1"; shift || true
    local out_dir="$1"; shift || true
    [ -d "$src_root" ] || return 0

    # Selectively collect certificates/keys/CRLs/configs/test vectors, etc.
    # Limit per-file size to avoid huge samples (e.g., larger than 512KB)
    local -a patterns=(
        "*.pem" "*.der" "*.crt" "*.cer" "*.key" "*.p12" "*.pfx" "*.crl"
        "*.conf" "*.ini" "*.cnf" "*.cfg" "*.txt" "*.json"
    )

    for pat in "${patterns[@]}"; do
        # Copy files while keeping only the basename (strip directory path)
        while IFS= read -r -d '' f; do
            base="$(basename "$f")"
            # De-duplicate: skip if a file with the same name already exists
            [ -e "$out_dir/$base" ] && continue
            cp -f "$f" "$out_dir/$base" 2>/dev/null || true
        done < <(find "$src_root" -type f -name "$pat" -size -512k -print0 2>/dev/null)
    done
}

for ARTEFACT_SUBDIR in "$ARTEFACTS_DIR"/*/; do
    [ -d "$ARTEFACT_SUBDIR" ] || continue
    NAME=$(basename "$ARTEFACT_SUBDIR")
    OUT_DIR="$CORPUS_DIR/$NAME"
    mkdir -p "$OUT_DIR"

    # Basic placeholder samples
    cat >"$OUT_DIR/min.json" <<'JSON'
{}
JSON

    cat >"$OUT_DIR/sample.json" <<'JSON'
{"a":1,"b":[true,false],"s":"x"}
JSON

    printf '\x00\x01\x02\x03' >"$OUT_DIR/min.bin"

    # Collect richer samples from the source tree
    copy_from_tree "$REPO_DIR/testing" "$OUT_DIR"
    copy_from_tree "$REPO_DIR/src/libstrongswan/tests" "$OUT_DIR"
    copy_from_tree "$REPO_DIR/src/pki/tests" "$OUT_DIR" || true
    copy_from_tree "$REPO_DIR/src/swanctl/tests" "$OUT_DIR" || true

    echo "Corpus generated: $OUT_DIR"
done


