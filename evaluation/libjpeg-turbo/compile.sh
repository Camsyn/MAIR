#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

rm -rf temp && mkdir temp

# 驱动是 C++
source ../activate_compile_flags.sh "$mode" true

pushd temp

SRC_DIR=../repo
mkdir -p .bin

cmake "$SRC_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_STATIC=1 -DENABLE_SHARED=0
make -j$(nproc)

# 编译 fuzzer 源（来自 fuzzbench 基准）
if [ ! -f "$SRC_DIR/libjpeg_turbo_fuzzer.cc" ]; then
  echo "错误: 未找到 $SRC_DIR/libjpeg_turbo_fuzzer.cc" >&2
  exit 1
fi
${CXX:-clang++} ${CXXFLAGS} -std=c++11 -I"$SRC_DIR/src" -Ibuild -c "$SRC_DIR/libjpeg_turbo_fuzzer.cc" -o ./fuzzer.o

# 选择静态库（优先 libjpeg*.a 或 libturbojpeg*.a）
JPEG_LIB=""
if ls libturbojpeg*.a >/dev/null 2>&1; then
  JPEG_LIB=$(ls libturbojpeg*.a | head -n1)
elif ls libjpeg*.a >/dev/null 2>&1; then
  JPEG_LIB=$(ls libjpeg*.a | head -n1)
elif ls *.a >/dev/null 2>&1; then
  JPEG_LIB=$(ls *.a | head -n1)
else
  echo "错误: 未找到静态库 (*.a)" >&2
  exit 1
fi

# 链接 fuzzer
${CXX:-clang++} ${CXXFLAGS} ./fuzzer.o -o .bin/libjpeg-turbo@libjpeg_turbo_fuzzer \
  ${LIB_FUZZING_ENGINE} "$JPEG_LIB" -lz -lm -lpthread -stdlib=libc++

popd

echo "完成: libjpeg-turbo($mode) 构建与产物收集。"