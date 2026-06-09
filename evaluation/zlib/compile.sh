#!/bin/bash
set -e  # Exit immediately if any command fails

rm -rf temp && mkdir temp

source ../activate_compile_flags.sh "$mode" true # is_cxx = true, as the driver is C++

pushd temp

cmake ../repo -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_SHARED=OFF
make -j all

mkdir -p .bin


set -x;

$CXX $CXXFLAGS $LDFLAGS -std=c++11 -I../repo ../zlib_uncompress_fuzzer.cc \
-o .bin/zlib@zlib_uncompress_fuzzer $LIB_FUZZING_ENGINE ./libz.a

cp test/static_minigzip .bin/minigzip

popd 
