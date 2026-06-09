#!/bin/bash
set -e  # Exit immediately if any command fails

rm -rf temp && mkdir temp

# The driver is C++
source ../activate_compile_flags.sh "$mode" true

pushd temp

SRC_DIR=../repo
mkdir -p .bin

# Use autotools to build a static libpng
"$SRC_DIR"/configure --disable-shared --enable-static
make -j$(nproc)

# Compile and link the fuzzer (use a C++ compiler to avoid missing libc++ symbols)
${CXX:-clang++} ${CXXFLAGS} -std=c++11 -I. -I"$SRC_DIR" -c "$SRC_DIR"/contrib/oss-fuzz/libpng_read_fuzzer.cc -o ./fuzzer.o

# Select the static library path (libtool artefacts are usually in .libs)
PNG_STATIC_LIB=""
if [ -f ./.libs/libpng16.a ]; then
  PNG_STATIC_LIB=./.libs/libpng16.a
elif ls ./.libs/libpng*.a >/dev/null 2>&1; then
  PNG_STATIC_LIB=$(ls ./.libs/libpng*.a | head -n1)
elif ls ./libpng*.a >/dev/null 2>&1; then
  PNG_STATIC_LIB=$(ls ./libpng*.a | head -n1)
else
  echo "Error: libpng static library not found" >&2
  exit 1
fi


${CXX:-clang++} ${CXXFLAGS} ./fuzzer.o -o .bin/libpng@ossfuzz \
  ${LIB_FUZZING_ENGINE} "$PNG_STATIC_LIB" -lz -lpthread -stdlib=libc++

echo $CXXFLAGS

popd 

echo "Done: libpng($mode) build and artefact collection."