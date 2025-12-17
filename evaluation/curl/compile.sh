#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

# 获取编译模式参数（支持环境变量覆盖）
mode="${1:-${mode:-raw}}"

# 目录规划：中间产物 temp/build-<mode>，最终产物 temp/.bin
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="${SCRIPT_DIR}/repo"
TEMP_DIR="${SCRIPT_DIR}/temp"
BUILD_DIR="${TEMP_DIR}"
BIN_DIR="${TEMP_DIR}/.bin"
rm -rf "${BUILD_DIR}" && mkdir -p "${BUILD_DIR}" "${BIN_DIR}"

# 驱动是 C++
source "${SCRIPT_DIR}/../activate_compile_flags.sh" "$mode" true

pushd "${BUILD_DIR}"

# 生成 configure 脚本并 out-of-tree 配置
pushd "$SRC_DIR" >/dev/null
autoreconf -fi || true
popd >/dev/null

CFLAGS="${CFLAGS}" CXXFLAGS="${CXXFLAGS}" LDFLAGS="${LDFLAGS}" \
"$SRC_DIR"/configure \
  --disable-shared --enable-static \
  --with-openssl --without-libpsl

make -j$(nproc)

# 编译并链接 fuzzer（嵌入模式签名，保证各模式二进制有差异）
INCLUDES=( -I. -I"$SRC_DIR"/include -I"$SRC_DIR"/tests/fuzzer )
${CXX:-xclang++} ${CXXFLAGS} "${INCLUDES[@]}" -c "$SRC_DIR"/tests/fuzzer/curl_fuzzer.cc -o ./fuzzer.o
${CXX:-xclang++} ${CXXFLAGS} "${INCLUDES[@]}" -c "$SRC_DIR"/tests/fuzzer/curl_fuzzer_callback.cc -o ./callback.o
${CXX:-xclang++} ${CXXFLAGS} "${INCLUDES[@]}" -c "$SRC_DIR"/tests/fuzzer/curl_fuzzer_tlv.cc -o ./tlv.o

cat > ./mode_tag.cc <<EOF
extern "C" const char kXsanModeTag[] = "xsan-mode=${mode}";
EOF
${CXX:-xclang++} ${CXXFLAGS} -c ./mode_tag.cc -o ./mode_tag.o

# 定位静态 libcurl
LIBCURL_A=""
if [ -f ./lib/.libs/libcurl.a ]; then
  LIBCURL_A=./lib/.libs/libcurl.a
elif [ -f "$SRC_DIR/lib/.libs/libcurl.a" ]; then
  LIBCURL_A="$SRC_DIR/lib/.libs/libcurl.a"
else
  LIBCURL_A=$(find . "$SRC_DIR" -path "*/lib/.libs/libcurl.a" | head -n1 || true)
fi
if [ -z "$LIBCURL_A" ]; then
  echo "未找到 libcurl.a" >&2; exit 1; fi

OUT_BIN="${BIN_DIR}/curl@curl_fuzzer"
${CXX:-xclang++} ${CXXFLAGS} ./fuzzer.o ./callback.o ./tlv.o ./mode_tag.o -o "${OUT_BIN}" \
  ${LIB_FUZZING_ENGINE} "$LIBCURL_A" -lssl -lcrypto -lz -lpthread ${LDFLAGS}

popd

echo "完成: curl($mode) 构建与产物收集 → ${BIN_DIR}/curl@curl_fuzzer.${mode}"
