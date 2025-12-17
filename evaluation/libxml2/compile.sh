#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

# 获取编译模式参数
mode="${1:-${mode:-raw}}"

rm -rf temp && mkdir temp

# 驱动是 C
source ../activate_compile_flags.sh "$mode" false

pushd temp

SRC_DIR=../repo
mkdir -p .bin

# 使用 CMake 构建 libxml2
echo "使用 CMake 构建 libxml2..."

cmake -B . -S "$SRC_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_SHARED_LIBS=OFF \
    -DLIBXML2_WITH_PYTHON=OFF \
    -DLIBXML2_WITH_THREADS=ON \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS} -lpthread"

cmake --build . -j$(nproc)

# 编译 fuzzer

# 检查 fuzzer 源文件是否存在
FUZZER_SRC="$SRC_DIR/fuzz/xml.c"
if [ ! -f "$FUZZER_SRC" ]; then
    echo "错误: 未找到 fuzzer 源文件: $FUZZER_SRC" >&2
    exit 1
fi

# 编译 fuzzer 辅助函数
$CC $CFLAGS -I"$SRC_DIR" -I"$SRC_DIR/include" -I"$SRC_DIR/fuzz" -I"." -c "$SRC_DIR/fuzz/fuzz.c" -o ./fuzz.o

# 编译 fuzzer
$CC $CFLAGS -I"$SRC_DIR" -I"$SRC_DIR/include" -I"$SRC_DIR/fuzz" -I"." -c "$FUZZER_SRC" -o ./fuzzer.o

# 链接 fuzzer
$CXX $CXXFLAGS ./fuzzer.o ./fuzz.o -o .bin/libxml2@xml_fuzzer \
    $LIB_FUZZING_ENGINE "./libxml2.a" -lm -lc++ -lpthread

popd

echo "完成: libxml2($mode) 构建与产物收集。"