#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

export PROJ=xz

rm -rf temp && mkdir temp

CONFIG_OPTIONS="--disable-shared --disable-doc --disable-scripts --disable-xzdec --disable-lzmadec --disable-lzma-links"
export SUBJECT=$PWD
export LDFLAGS=-lpthread
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# 模式：优先位置参数，其次环境变量，默认 raw
mode="${1:-${mode:-raw}}"

source ../activate_compile_flags.sh "$mode" false

# 首先生成configure脚本
pushd repo
if [ ! -f "configure" ]; then
    echo "生成configure脚本..."
    ./autogen.sh
fi
popd

pushd temp
rm -rf *
../repo/configure $CONFIG_OPTIONS
make clean
make -j

# 将 xz 下的指定可执行文件移动到 ../artefacts 目录
mkdir -p .bin
XZ_BINARIES=(xz)
for bin in "${XZ_BINARIES[@]}"; do
    if [ -f "src/xz/$bin" ]; then
        mv "src/xz/$bin" ".bin/$bin"
    fi
done

popd