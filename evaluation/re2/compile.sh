#!/bin/bash
set -e  # Exit immediately if any command fails

# Get build mode (support positional argument or environment variable)
mode="${1:-${mode:-raw}}"

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="${SCRIPT_DIR}/repo"
TEMP_DIR="${SCRIPT_DIR}/temp"
BUILD_DIR="${TEMP_DIR}"
BIN_DIR="${TEMP_DIR}/.bin"
rm -rf "${BUILD_DIR}" && mkdir -p "${BUILD_DIR}" "${BIN_DIR}"

# Driver is C++
source "${SCRIPT_DIR}/../activate_compile_flags.sh" "$mode" true

pushd "${BUILD_DIR}"

# At this point SRC_DIR is already an absolute path

# Enable C++17 everywhere (required by Abseil and libraries using it)
export CXXFLAGS="${CXXFLAGS} -std=c++17"
export LDFLAGS="${LDFLAGS} -stdlib=libc++"

# 1) Build and install Abseil following the oss-fuzz flow
# Use locally built Abseil to avoid polluting the system and include current mode flags
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

# 2) Build RE2 static library (injecting current mode flags, without installing)
make -C "$SRC_DIR" clean || true
make -C "$SRC_DIR" CXXFLAGS="${CXXFLAGS}" LDFLAGS="${LDFLAGS}" -j$(nproc) obj/libre2.a

if [ ! -f "$SRC_DIR/obj/libre2.a" ]; then
  echo "Error: failed to build libre2.a" >&2
  exit 1
fi

# 3) Build and link the fuzzer (use RE2's own re2/fuzzing/re2_fuzzer.cc)
if [ ! -f "$SRC_DIR/re2/fuzzing/re2_fuzzer.cc" ]; then
  echo "Error: $SRC_DIR/re2/fuzzing/re2_fuzzer.cc not found" >&2
  exit 1
fi

# Ensure the fuzzer uses the same sanitizer flags as RE2 and embeds a mode signature to distinguish artefacts
FUZZER_CXXFLAGS="${CXXFLAGS} -DXSAN_MODE_STR=\"${mode}\""
FUZZER_LDFLAGS="${LDFLAGS}"

${CXX:-xclang++} ${FUZZER_CXXFLAGS} -I"$SRC_DIR" -c "$SRC_DIR/re2/fuzzing/re2_fuzzer.cc" -o ./fuzzer.o

# Compile an extra object containing the mode string so binaries for different modes visibly differ
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
