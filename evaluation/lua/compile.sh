#!/bin/bash
set -e

# Usage: ./compile.sh <mode>
mode="${1:-${mode:-raw}}"

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/repo"
OUT_DIR="$SCRIPT_DIR/temp"
OUT_BIN_DIR="$OUT_DIR/.bin"
OUT_BIN="$OUT_BIN_DIR/lua"

mkdir -p "$OUT_BIN_DIR"
cp -r "$SRC_DIR"/* "$OUT_DIR"

# Inject compile/link flags (C project, second parameter false)
source "$SCRIPT_DIR/../activate_compile_flags.sh" "$mode" false

# Lua top-level Makefile: use the default target directly
make -C "$OUT_DIR" clean || true
make -C "$OUT_DIR" -j"$(nproc)" \
  CC="${CC:-xclang}" \
  MYCFLAGS="$CFLAGS" \
  MYLDFLAGS="$LDFLAGS" 2>&1 | cat

if [ -x "$OUT_DIR/lua" ]; then
  cp -f "$OUT_DIR/lua" "$OUT_BIN"
  echo "built: $OUT_BIN"
else
  echo "lua executable not found" >&2
  exit 1
fi

