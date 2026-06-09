#!/bin/bash
set -e  # Exit immediately if any command fails

rm -rf temp && mkdir temp

source ../activate_compile_flags.sh "$mode" true # is_cxx = true, as the driver is C++

pushd temp

cmake -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
      -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF -DJSONCPP_WITH_TESTS=OFF \
      -DBUILD_SHARED_LIBS=OFF -G "Unix Makefiles" ../repo
make -j

mkdir -p .bin

# Compile fuzzer.
$CXX $CXXFLAGS -I../repo/include $LIB_FUZZING_ENGINE \
    ../repo/src/test_lib_json/fuzz.cpp -o .bin/jsoncpp@fuzz \
    lib/libjsoncpp.a

popd 


