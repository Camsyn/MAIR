#!/bin/bash
set -e

mkdir -p repo artefacts/lua corpus/lua temp/.bin

if [ ! -d repo/.git ]; then
  git clone https://github.com/lua/lua.git repo --depth 1 --branch master
else
  (cd repo && git pull --ff-only)
fi


# The upstream repository does not provide test files; generate corpus manually
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_lua_corpus.sh" ]; then
    "$SCRIPT_DIR/create_lua_corpus.sh" || true
else
    echo "Warning: executable create_lua_corpus.sh not found, skipping corpus generation." >&2
fi



