#!/bin/bash
set -e

# Follow the sqlite3-style template: fetch source and fuzzer files from git.
export PROJ="lcms"

# lcms2 source repository
SRC_URL="https://github.com/mm2/Little-CMS.git"

echo "Cloning lcms source..."
if [ ! -d "repo" ]; then
    git clone "$SRC_URL" repo --depth 1
else
    pushd repo >/dev/null
    git pull --ff-only || true
    popd >/dev/null
fi

# Search the repository to construct a corpus
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_lcms_corpus.sh" ]; then
    # If the script is not executable, try to chmod +x; if that still fails, run via bash.
    if [ ! -x "$SCRIPT_DIR/create_lcms_corpus.sh" ]; then
        chmod +x "$SCRIPT_DIR/create_lcms_corpus.sh" 2>/dev/null || true
    fi
    if [ -x "$SCRIPT_DIR/create_lcms_corpus.sh" ]; then
        "$SCRIPT_DIR/create_lcms_corpus.sh" || true
    else
        bash "$SCRIPT_DIR/create_lcms_corpus.sh" || true
    fi
else
    echo "Warning: create_lcms_corpus.sh not found, skipping corpus generation" >&2
fi

rm -rf oss-fuzz-temp fuzzbench-temp