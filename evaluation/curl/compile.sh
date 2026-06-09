#!/bin/bash
set -e  # Exit immediately if any command fails

# Get compilation mode parameter (allows environment variable override)
mode="${1:-${mode:-raw}}"

# Directory layout: intermediate artefacts in temp/build-<mode>, final binaries in temp/.bin
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="${SCRIPT_DIR}/repo"
TEMP_DIR="${SCRIPT_DIR}/temp"
BUILD_DIR="${TEMP_DIR}"
BIN_DIR="${TEMP_DIR}/.bin"
rm -rf "${BUILD_DIR}" && mkdir -p "${BUILD_DIR}" "${BIN_DIR}"

# The driver is C++
source "${SCRIPT_DIR}/../activate_compile_flags.sh" "$mode" true

pushd "${BUILD_DIR}"

# Generate configure script and perform out-of-tree configuration
pushd "$SRC_DIR" >/dev/null
autoreconf -fi || true
popd >/dev/null

CFLAGS="${CFLAGS}" CXXFLAGS="${CXXFLAGS}" LDFLAGS="${LDFLAGS}" \
"$SRC_DIR"/configure \
  --disable-shared --enable-static \
  --with-openssl --without-libpsl

make -j$(nproc)

# Compile and link the fuzzer (embed mode signature so binaries differ across modes)
INCLUDES=( -I. -I"$SRC_DIR"/include -I"$SRC_DIR"/tests/fuzzer )
${CXX:-xclang++} ${CXXFLAGS} "${INCLUDES[@]}" -c "$SRC_DIR"/tests/fuzzer/curl_fuzzer.cc -o ./fuzzer.o
${CXX:-xclang++} ${CXXFLAGS} "${INCLUDES[@]}" -c "$SRC_DIR"/tests/fuzzer/curl_fuzzer_callback.cc -o ./callback.o
${CXX:-xclang++} ${CXXFLAGS} "${INCLUDES[@]}" -c "$SRC_DIR"/tests/fuzzer/curl_fuzzer_tlv.cc -o ./tlv.o

cat > ./mode_tag.cc <<EOF
extern "C" const char kXsanModeTag[] = "xsan-mode=${mode}";
EOF
${CXX:-xclang++} ${CXXFLAGS} -c ./mode_tag.cc -o ./mode_tag.o

# Locate static libcurl
LIBCURL_A=""
if [ -f ./lib/.libs/libcurl.a ]; then
  LIBCURL_A=./lib/.libs/libcurl.a
elif [ -f "$SRC_DIR/lib/.libs/libcurl.a" ]; then
  LIBCURL_A="$SRC_DIR/lib/.libs/libcurl.a"
else
  LIBCURL_A=$(find . "$SRC_DIR" -path "*/lib/.libs/libcurl.a" | head -n1 || true)
fi
if [ -z "$LIBCURL_A" ]; then
  echo "libcurl.a not found" >&2; exit 1; fi

OUT_BIN="${BIN_DIR}/curl@curl_fuzzer"
${CXX:-xclang++} ${CXXFLAGS} ./fuzzer.o ./callback.o ./tlv.o ./mode_tag.o -o "${OUT_BIN}" \
  ${LIB_FUZZING_ENGINE} "$LIBCURL_A" -lssl -lcrypto -lz -lpthread ${LDFLAGS}

popd

echo "Done: curl($mode) build and artefact collection → ${BIN_DIR}/curl@curl_fuzzer.${mode}"
