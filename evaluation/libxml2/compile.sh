#!/bin/bash
set -e  # Exit immediately if any command fails

# Get compilation mode parameter
mode="${1:-${mode:-raw}}"

rm -rf temp && mkdir temp

# The driver is C
source ../activate_compile_flags.sh "$mode" false

pushd temp

SRC_DIR=../repo
mkdir -p .bin

# Build libxml2 with CMake
echo "Building libxml2 with CMake..."

cmake -B . -S "$SRC_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_SHARED_LIBS=OFF \
    -DLIBXML2_WITH_PYTHON=OFF \
    -DLIBXML2_WITH_THREADS=ON \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS} -lpthread"

cmake --build . -j$(nproc)

# Build the fuzzer

# Check whether the fuzzer source file exists
FUZZER_SRC="$SRC_DIR/fuzz/xml.c"
if [ ! -f "$FUZZER_SRC" ]; then
    echo "Error: fuzzer source file not found: $FUZZER_SRC" >&2
    exit 1
fi

# Compile fuzzer helper functions
$CC $CFLAGS -I"$SRC_DIR" -I"$SRC_DIR/include" -I"$SRC_DIR/fuzz" -I"." -c "$SRC_DIR/fuzz/fuzz.c" -o ./fuzz.o

# Compile the fuzzer
$CC $CFLAGS -I"$SRC_DIR" -I"$SRC_DIR/include" -I"$SRC_DIR/fuzz" -I"." -c "$FUZZER_SRC" -o ./fuzzer.o

# Link the fuzzer
$CXX $CXXFLAGS ./fuzzer.o ./fuzz.o -o .bin/libxml2@xml_fuzzer \
    $LIB_FUZZING_ENGINE "./libxml2.a" -lm -lc++ -lpthread

popd

echo "Done: libxml2($mode) build and artefact collection."