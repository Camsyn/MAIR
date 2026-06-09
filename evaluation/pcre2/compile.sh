#!/bin/bash
set -e  # Exit immediately if any command fails

export PROJ=pcre2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

pushd $SCRIPT_DIR

rm -rf temp && mkdir temp


source ../activate_compile_flags.sh $mode false

pushd temp
rm -rf *
cmake ../repo -DBUILD_SHARED_LIBS=OFF
cmake --build . -j$(nproc)

# Move specified executables into the ../artefacts directory
mkdir -p .bin
BINARIES=(pcre2grep pcre2test)
for bin in "${BINARIES[@]}"; do
    if [ -f "$bin" ]; then
        mv "$bin" ".bin/$bin"
    fi
done

popd

popd