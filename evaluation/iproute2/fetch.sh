#!/bin/bash
set -euo pipefail
export PROJ=iproute2
URL="https://github.com/iproute2/iproute2.git"
if [ ! -d repo/.git ]; then 
    git clone "$URL" repo --depth 1
else 
    echo "repo already exists, skipping clone"
fi

# The built tools do not need program inputs, so fetch related documentation as corpus files.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -x "$SCRIPT_DIR/create_iproute2_corpus.sh" ]; then
    "$SCRIPT_DIR/create_iproute2_corpus.sh" || true
else
    echo "Warning: Executable create_iproute2_corpus.sh not found, skipping corpus generation" >&2
fi

