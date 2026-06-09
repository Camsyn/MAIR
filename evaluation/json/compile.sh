#!/bin/bash
set -e  # Exit immediately if any command fails

export PROJ=pcre2


rm -rf temp && mkdir temp

source ../activate_compile_flags.sh $mode true


# This is a C++ header-only library project, so the build procedure is special
# This directory contains the fuzz interfaces for json
mkdir -p temp/.bin
pushd repo/tests

# 1. We only instrument the project itself, not libc++.so.
# 2. parse_afl_fuzzer and parse_bson_fuzzer call std::to_string, which is defined in string.cpp@libc++.
# 3. Therefore MSan may report false positives, thinking the std::string returned by std::to_string is uninitialized.
# 4. MSan reports add significant overhead, so for fairness we exclude parse_afl_fuzzer and parse_bson_fuzzer.
BINARIES=(
    parse_afl_fuzzer
    parse_bson_fuzzer
    parse_cbor_fuzzer 
    parse_msgpack_fuzzer 
    parse_ubjson_fuzzer 
    parse_bjdata_fuzzer
)

# The following compilation commands are equivalent to make -j ${BINARIES[@]}
# We execute them manually to collect the compile time (*.json with -ftime-trace).
# make -j ${BINARIES[@]}

CPPFLAGS=" -std=c++11 -I ../single_include "

# First compile each fuzzer object file into ../../temp, then link to generate executables.
# Compile targets in parallel and wait for all object compilations to finish.

# 1. Compile object files in parallel
compile_jobs=()
$CXX $CXXFLAGS $CPPFLAGS -c src/fuzzer-driver_afl.cpp   -o ../../temp/fuzzer-driver_afl.o   &
compile_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS -c src/fuzzer-parse_json.cpp    -o ../../temp/fuzzer-parse_json.o   &
compile_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS -c src/fuzzer-parse_bson.cpp    -o ../../temp/fuzzer-parse_bson.o   &
compile_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS -c src/fuzzer-parse_cbor.cpp    -o ../../temp/fuzzer-parse_cbor.o   &
compile_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS -c src/fuzzer-parse_msgpack.cpp -o ../../temp/fuzzer-parse_msgpack.o &
compile_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS -c src/fuzzer-parse_ubjson.cpp  -o ../../temp/fuzzer-parse_ubjson.o  &
compile_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS -c src/fuzzer-parse_bjdata.cpp  -o ../../temp/fuzzer-parse_bjdata.o  &
compile_jobs+=($!)
# Wait for all object compilations to complete
for job in "${compile_jobs[@]}"; do
    wait $job
done

# 2. Link executables in parallel
link_jobs=()
$CXX $CXXFLAGS $CPPFLAGS ../../temp/fuzzer-driver_afl.o ../../temp/fuzzer-parse_json.o    $LDFLAGS -o parse_afl_fuzzer    &
link_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS ../../temp/fuzzer-driver_afl.o ../../temp/fuzzer-parse_bson.o    $LDFLAGS -o parse_bson_fuzzer   &
link_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS ../../temp/fuzzer-driver_afl.o ../../temp/fuzzer-parse_cbor.o    $LDFLAGS -o parse_cbor_fuzzer   &
link_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS ../../temp/fuzzer-driver_afl.o ../../temp/fuzzer-parse_msgpack.o $LDFLAGS -o parse_msgpack_fuzzer &
link_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS ../../temp/fuzzer-driver_afl.o ../../temp/fuzzer-parse_ubjson.o  $LDFLAGS -o parse_ubjson_fuzzer &
link_jobs+=($!)
$CXX $CXXFLAGS $CPPFLAGS ../../temp/fuzzer-driver_afl.o ../../temp/fuzzer-parse_bjdata.o  $LDFLAGS -o parse_bjdata_fuzzer &
link_jobs+=($!)
# Wait for all link jobs to finish
for job in "${link_jobs[@]}"; do
    wait $job || exit 1
done

for bin in "${BINARIES[@]}"; do
    if [ -f "$bin" ]; then
        mv "$bin" "../../temp/.bin/json@$bin"
    fi
done

popd

