#!/bin/bash
set -e  # Exit immediately if any command fails

export PROJ=pcre2


rm -rf temp && mkdir temp

source ../activate_compile_flags.sh $mode false

pushd temp
rm -rf *
cmake ../repo -DBUILD_SHARED_LIBS=OFF
cmake --build . -j$(nproc)

# Move the built fuzzing executable into the ../artefacts directory
mkdir -p .bin
cp fuzzing/fuzz_main .bin/cJSON@fuzz_main

popd
