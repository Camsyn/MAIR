#!/bin/bash
set -e

# 参照 sqlite3 模板：构建静态库，再单独编译 fuzzer 并链接
mode="${1:-${mode:-raw}}"

rm -rf temp && mkdir -p temp

# 驱动为 C++（libFuzzer/AFL++），开启统一编译标志
source ../activate_compile_flags.sh "$mode" true

pushd temp >/dev/null

SRC_DIR=../repo
mkdir -p .bin

# 使用 autotools/cmake 之一。优先 autotools 的 out-of-tree 构建
if [ -f "$SRC_DIR/configure" ]; then
    # 确保源码目录未被就地配置过
    make -C "$SRC_DIR" distclean >/dev/null 2>&1 || true
    # 将当前模式的编译/链接标志传入 autotools 配置（out-of-tree）
    CFLAGS="$CFLAGS" \
    CXXFLAGS="$CXXFLAGS" \
    LDFLAGS="$LDFLAGS" \
    "$SRC_DIR"/configure --disable-shared --enable-static
else
    # 回退到 CMake
    cmake -S "$SRC_DIR" -B build \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_C_FLAGS="$CFLAGS" \
      -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS"
    cmake --build build -j$(nproc)
fi

make -j$(nproc) || true

# 定位静态库
LIB_STATIC=""
if [ -f "liblcms2.a" ]; then
    LIB_STATIC=./liblcms2.a
elif [ -f "lib/.libs/liblcms2.a" ]; then
    LIB_STATIC=./lib/.libs/liblcms2.a
elif [ -f "$SRC_DIR/src/.libs/liblcms2.a" ]; then
    LIB_STATIC="$SRC_DIR/src/.libs/liblcms2.a"
else
    # 在树内搜寻
    LIB_STATIC=$(find . "$SRC_DIR" -name "liblcms2.a" | head -n1 || true)
fi

# 编译 fuzzer 源, the driver comes from fuzzbench
FUZZ_CC="$SRC_DIR/../cms_transform_fuzzer.cc"
if [ ! -f "$FUZZ_CC" ]; then
    echo "未找到 fuzzer 源文件: $FUZZ_CC" >&2
    exit 1
fi

$CXX $CXXFLAGS -I"$SRC_DIR/include"  -c $FUZZ_CC -o ./fuzzer.o

# 链接 fuzzer 可执行文件
if [ -z "$LIB_STATIC" ]; then
    echo "未找到 liblcms2.a，无法链接" >&2
    exit 1
fi

$CXX $CXXFLAGS ./fuzzer.o -o .bin/lcms@cms_transform_fuzzer \
    "$LIB_STATIC" $LIB_FUZZING_ENGINE -lz -lm $LDFLAGS

popd >/dev/null

echo "完成: lcms($mode) 构建与产物收集。"
