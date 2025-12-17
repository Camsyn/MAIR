#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

export PROJ=binutils-gdb


rm -rf temp && mkdir temp

CONFIG_OPTIONS="--disable-shared --disable-gdb \
                 --disable-libdecnumber --disable-readline \
                 --disable-sim --disable-ld"
export SUBJECT=$PWD
export LDFLAGS=-lpthread
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

source ../activate_compile_flags.sh $mode false

pushd temp
rm -rf *
../repo/configure $CONFIG_OPTIONS
# cmake ../$PROJ/ -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_CXX_FLAGS="$ADDITIONAL" -DBUILD_SHARED_LIBS=OFF
make clean
make -j

# 将 binutils/ 下的指定可执行文件移动到 ../artefacts 目录
mkdir -p .bin
BINUTILS_BINARIES=(nm-new readelf objdump objcopy strings size cxxfilt strip-new)
for bin in "${BINUTILS_BINARIES[@]}"; do
    if [ -f "binutils/$bin" ]; then
        mv "binutils/$bin" ".bin/$bin"
    fi
done

popd
