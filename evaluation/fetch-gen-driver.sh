#!/bin/bash

# Use BASH_SOURCE to get the real script path, compatible with both source and direct execution
SCRIPTS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd)

pushd ${SCRIPTS_DIR}

# Download and compile AFL v2.57b.
# Set AFL_NO_X86 to skip flaky tests.
git clone \
        --depth 1 \
        --branch v2.57b \
        https://github.com/google/AFL.git afl

wget https://raw.githubusercontent.com/llvm/llvm-project/5feb80e748924606531ba28c97fe65145c65372e/compiler-rt/lib/fuzzer/afl/afl_driver.cpp -O afl_driver.cpp

# Avoid using std::cout to eliminate the FPs of MSan, as we don't instrument libc++.
sed -i 's/std::cout << "Execution successful" << std::endl;/Printf("Execution successful\\n");/g' afl_driver.cpp
sed -i 's/std::cout << "Reading " << length << " bytes from " << argv\[i\] << std::endl;/Printf("Reading %zd bytes from %s\\n", length, argv[i]);/g' afl_driver.cpp


pushd afl
# Use afl_driver.cpp from LLVM as our fuzzing library.
clang -Wno-pointer-sign -g -c llvm_mode/afl-llvm-rt.o.c -Iafl
clang++ -stdlib=libc++ -std=c++11 -O2 -g -c ../afl_driver.cpp
ar r ../libAFL.a *.o
popd

rm -rf afl

popd
