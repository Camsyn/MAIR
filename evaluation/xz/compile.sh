#!/bin/bash
set -e  # Exit immediately if any command fails

export PROJ=xz

rm -rf temp && mkdir temp

CONFIG_OPTIONS="--disable-shared --disable-doc --disable-scripts --disable-xzdec --disable-lzmadec --disable-lzma-links"
export SUBJECT=$PWD
export LDFLAGS=-lpthread
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# Mode: prefer positional argument, then environment variable, default to raw
mode="${1:-${mode:-raw}}"

source ../activate_compile_flags.sh "$mode" false

# First generate the configure script if needed
pushd repo
if [ ! -f "configure" ]; then
    echo "Generating configure script..."
    ./autogen.sh
fi
popd

pushd temp
rm -rf *
../repo/configure $CONFIG_OPTIONS
make clean
make -j

# Move selected xz binaries into ../artefacts directory
mkdir -p .bin
XZ_BINARIES=(xz)
for bin in "${XZ_BINARIES[@]}"; do
    if [ -f "src/xz/$bin" ]; then
        mv "src/xz/$bin" ".bin/$bin"
    fi
done

popd