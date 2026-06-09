#!/bin/bash
export PROJ=xz
URL="https://github.com/tukaani-project/xz.git"

# Use depth 1 to avoid cloning the history
git clone "$URL" repo --depth 1

# Create initial xz corpus
mkdir -p corpus/xz
cp repo/testdata/xz* corpus/xz

# Search the repository for additional corpus files
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_xz_corpus.sh" ]; then
    "$SCRIPT_DIR/create_xz_corpus.sh" || true
else
    echo "Warning: Executable create_xz_corpus.sh not found, skipping corpus generation" >&2
fi  



