#!/bin/bash
set -e

# Follow the sqlite3 template: build a static library, then separately compile and link the fuzzer
mode="${1:-${mode:-raw}}"

rm -rf temp && mkdir -p temp

# The driver is C++ (libFuzzer/AFL++), enable unified compilation flags
source ../activate_compile_flags.sh "$mode" true

pushd temp >/dev/null

SRC_DIR=../repo
mkdir -p .bin

# Use either autotools or CMake. Prefer autotools with an out-of-tree build
if [ -f "$SRC_DIR/configure" ]; then
    # Ensure the source directory is not configured in-place
    make -C "$SRC_DIR" distclean >/dev/null 2>&1 || true
    # Pass current compilation/link flags into the autotools configuration (out-of-tree)
    CFLAGS="$CFLAGS" \
    CXXFLAGS="$CXXFLAGS" \
    LDFLAGS="$LDFLAGS" \
    "$SRC_DIR"/configure --disable-shared --enable-static
else
    # Fall back to CMake
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

# Locate the static library
LIB_STATIC=""
if [ -f "liblcms2.a" ]; then
    LIB_STATIC=./liblcms2.a
elif [ -f "lib/.libs/liblcms2.a" ]; then
    LIB_STATIC=./lib/.libs/liblcms2.a
elif [ -f "$SRC_DIR/src/.libs/liblcms2.a" ]; then
    LIB_STATIC="$SRC_DIR/src/.libs/liblcms2.a"
else
    # Search within the tree
    LIB_STATIC=$(find . "$SRC_DIR" -name "liblcms2.a" | head -n1 || true)
fi

# Compile the fuzzer source; the driver comes from fuzzbench
FUZZ_CC="$SRC_DIR/../cms_transform_fuzzer.cc"
if [ ! -f "$FUZZ_CC" ]; then
    echo "Fuzzer source not found: $FUZZ_CC" >&2
    exit 1
fi

$CXX $CXXFLAGS -I"$SRC_DIR/include"  -c $FUZZ_CC -o ./fuzzer.o

# Link the fuzzer executable
if [ -z "$LIB_STATIC" ]; then
    echo "liblcms2.a not found, cannot link" >&2
    exit 1
fi

$CXX $CXXFLAGS ./fuzzer.o -o .bin/lcms@cms_transform_fuzzer \
    "$LIB_STATIC" $LIB_FUZZING_ENGINE -lz -lm $LDFLAGS

popd >/dev/null

echo "Done: lcms($mode) build and artefact collection."
