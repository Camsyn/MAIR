#!/bin/bash
set -e  # Exit immediately if any command fails

rm -rf temp && mkdir temp

# The driver is C++
source ../activate_compile_flags.sh "$mode" true

pushd temp

SRC_DIR=../repo
mkdir -p .bin

cmake "$SRC_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_STATIC=1 -DENABLE_SHARED=0
make -j$(nproc)

# Compile the fuzzer source (from the fuzzbench benchmark)
if [ ! -f "$SRC_DIR/libjpeg_turbo_fuzzer.cc" ]; then
  echo "Error: $SRC_DIR/libjpeg_turbo_fuzzer.cc not found" >&2
  exit 1
fi
${CXX:-clang++} ${CXXFLAGS} -std=c++11 -I"$SRC_DIR/src" -I. -c "$SRC_DIR/libjpeg_turbo_fuzzer.cc" -o ./fuzzer.o

# Select a static library (prefer libjpeg*.a or libturbojpeg*.a)
JPEG_LIB=""
if ls libturbojpeg*.a >/dev/null 2>&1; then
  JPEG_LIB=$(ls libturbojpeg*.a | head -n1)
elif ls libjpeg*.a >/dev/null 2>&1; then
  JPEG_LIB=$(ls libjpeg*.a | head -n1)
elif ls *.a >/dev/null 2>&1; then
  JPEG_LIB=$(ls *.a | head -n1)
else
  echo "Error: no static library (*.a) found" >&2
  exit 1
fi

# Link the fuzzer
${CXX:-clang++} ${CXXFLAGS} ./fuzzer.o -o .bin/libjpeg-turbo@libjpeg_turbo_fuzzer \
  ${LIB_FUZZING_ENGINE} "$JPEG_LIB" -lz -lm -lpthread -stdlib=libc++

popd

echo "Done: libjpeg-turbo($mode) build and artefact collection."