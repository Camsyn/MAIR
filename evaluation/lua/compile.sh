#!/bin/bash
set -e

# 用法: ./compile.sh <mode>
mode="${1:-${mode:-raw}}"

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/repo"
OUT_DIR="$SCRIPT_DIR/temp"
OUT_BIN_DIR="$OUT_DIR/.bin"
OUT_BIN="$OUT_BIN_DIR/lua"

mkdir -p "$OUT_BIN_DIR"
cp -r "$SRC_DIR"/* "$OUT_DIR"

# 注入编译/链接标志（C 项目，第二参 false）
source "$SCRIPT_DIR/../activate_compile_flags.sh" "$mode" false

# Lua 顶层 makefile：直接使用默认目标
make -C "$OUT_DIR" clean || true
make -C "$OUT_DIR" -j"$(nproc)" \
  CC="${CC:-xclang}" \
  MYCFLAGS="$CFLAGS" \
  MYLDFLAGS="$LDFLAGS" 2>&1 | cat

if [ -x "$OUT_DIR/lua" ]; then
  cp -f "$OUT_DIR/lua" "$OUT_BIN"
  echo "built: $OUT_BIN"
else
  echo "未找到 lua 可执行文件" >&2
  exit 1
fi

