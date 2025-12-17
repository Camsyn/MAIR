#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

export PROJ=iproute2

# 使用与 tpm2-tss 相同的整体流程与变量约定
export SUBJECT=$PWD
export LDFLAGS="${LDFLAGS:+$LDFLAGS }-lpthread"
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# 模式：优先位置参数，其次环境变量，默认 raw
mode="${1:-${mode:-raw}}"

# 激活编译标志（C 项目，第二参为 false）
source ../activate_compile_flags.sh "$mode" false

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR="$ROOT_DIR/repo"
BUILD_DIR="$ROOT_DIR/temp"

rm -rf "$BUILD_DIR" && mkdir "$BUILD_DIR"

# 复制源码到构建目录
cp -r "$REPO_DIR"/* "$BUILD_DIR/"

pushd "$BUILD_DIR"

# 生成 config.mk
if [ ! -f "config.mk" ]; then
    echo "生成 config.mk..."
    ./configure
fi

# 清理之前的构建
make clean || true

# 编译
make -j

# 在构建目录收集到 .bin
mkdir -p .bin

# iproute2 主要工具
CANDIDATES=(
  ip
  tc
  ss
  bridge
  devlink
  genl
  rdma
  tipc
  vdpa
  arpd
  nstat
  rtacct
  lnstat
  ifstat
)

SEARCH_PATHS=(
  "./ip"
  "./tc"
  "./misc"
  "./bridge"
  "./devlink"
  "./genl"
  "./rdma"
  "./tipc"
  "./vdpa"
  "./misc/arpd"
  "./misc/nstat"
  "./misc/rtacct"
  "./misc/lnstat"
  "./misc/rtstat"
)

found_any=0
for name in "${CANDIDATES[@]}"; do
  for dir in "${SEARCH_PATHS[@]}"; do
    if [ -f "$dir/$name" ]; then
      cp "$dir/$name" ".bin/$name"
      found_any=1
      break
    fi
  done
done

if [ "$found_any" -eq 0 ]; then
  echo "警告: 未在预期路径发现 iproute2 可执行文件。" >&2
fi

# 兜底搜索：抓取当前目录下所有可执行文件
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find . -maxdepth 2 -type f -perm -111 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# 同步到 artefacts：为每个可执行生成 <name>.<mode>
for f in .bin/*; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  out_dir="../artefacts/$name"
  mkdir -p "$out_dir"
  cp -f "$f" "$out_dir/${name}.${mode}"
  chmod +x "$out_dir/${name}.${mode}" || true
done

popd

echo "完成: iproute2($mode) 构建与产物收集。"
