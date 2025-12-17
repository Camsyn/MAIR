#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

# 获取编译模式参数（支持参数或环境变量）
mode="${1:-${mode:-raw}}"

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="${SCRIPT_DIR}/repo"
TEMP_DIR="${SCRIPT_DIR}/temp"
BUILD_DIR="${TEMP_DIR}"
BIN_DIR="${TEMP_DIR}/.bin"
rm -rf "${BUILD_DIR}" && mkdir -p "${BUILD_DIR}" "${BIN_DIR}"

# 驱动是 C++
source "${SCRIPT_DIR}/../activate_compile_flags.sh" "$mode" true

pushd "${BUILD_DIR}"

# 这里的 SRC_DIR 已经是绝对路径

# 统一启用 C++17（Abseil 和使用 Abseil 的库都需要）
export CXXFLAGS="${CXXFLAGS} -std=c++17"
export LDFLAGS="${LDFLAGS} -stdlib=libc++"

# 1) 构建并安装 Abseil（按 oss-fuzz 流程）
# 使用本地构建的 Abseil（避免污染系统），并确保加入当前模式标志
if [ ! -d ../repo/abseil-cpp ]; then
  git clone https://github.com/abseil/abseil-cpp.git --depth 1 ../repo/abseil-cpp
fi
mkdir -p absl-build && pushd absl-build
cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_C_COMPILER="${CC}" -DCMAKE_CXX_COMPILER="${CXX}" \
      -DCMAKE_C_FLAGS="${CFLAGS}" -DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
      -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS}" \
      -DCMAKE_INSTALL_PREFIX="$(pwd)/../absl-install" \
      ../../repo/abseil-cpp
make -j$(nproc)
make install
popd
export PKG_CONFIG_PATH="$(pwd)/absl-install/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# 2) 构建 RE2 静态库（注入当前模式标志，不安装到系统）
make -C "$SRC_DIR" clean || true
make -C "$SRC_DIR" CXXFLAGS="${CXXFLAGS}" LDFLAGS="${LDFLAGS}" -j$(nproc) obj/libre2.a

if [ ! -f "$SRC_DIR/obj/libre2.a" ]; then
  echo "错误: 构建 libre2.a 失败" >&2
  exit 1
fi

# 3) 编译并链接 fuzzer（使用 RE2 自带的 re2/fuzzing/re2_fuzzer.cc）
if [ ! -f "$SRC_DIR/re2/fuzzing/re2_fuzzer.cc" ]; then
  echo "错误: 未找到 $SRC_DIR/re2/fuzzing/re2_fuzzer.cc" >&2
  exit 1
fi

# 确保 fuzzer 使用与 RE2 相同的 sanitizer 标志，并嵌入模式签名以便区分产物
FUZZER_CXXFLAGS="${CXXFLAGS} -DXSAN_MODE_STR=\"${mode}\""
FUZZER_LDFLAGS="${LDFLAGS}"

${CXX:-xclang++} ${FUZZER_CXXFLAGS} -I"$SRC_DIR" -c "$SRC_DIR/re2/fuzzing/re2_fuzzer.cc" -o ./fuzzer.o

# 额外编译一个包含模式字符串的目标，确保不同模式二进制有可见差异
cat > ./mode_tag.cc <<EOF
extern "C" const char kXsanModeTag[] = "xsan-mode=${mode}";
EOF
${CXX:-xclang++} ${FUZZER_CXXFLAGS} -c ./mode_tag.cc -o ./mode_tag.o

PKG_LIBS=$(pkg-config re2 --libs 2>/dev/null | sed -e 's/-lre2//' || true)

OUT_BIN="${BIN_DIR}/re2@re2_fuzzer"
${CXX:-xclang++} ${FUZZER_CXXFLAGS} ./fuzzer.o ./mode_tag.o -o "${OUT_BIN}" \
  ${LIB_FUZZING_ENGINE} "$SRC_DIR/obj/libre2.a" ${PKG_LIBS} ${FUZZER_LDFLAGS} -lpthread -stdlib=libc++

chmod +x "${OUT_BIN}"

popd
