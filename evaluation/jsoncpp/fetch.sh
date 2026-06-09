#!/bin/bash

set -euo pipefail
export PROJ=iproute2
URL="https://github.com/open-source-parsers/jsoncpp"

if [ ! -d repo/.git ]; then 
    git clone "$URL" repo --depth 1 --branch master
    pushd repo
    
    # Benchmark from FuzzBench; the version follows fuzzbench/benchmarks/*/benchmark.yaml
    git fetch --depth 1 origin 8190e061bc2d95da37479a638aa2c9e483e58ec6
    git checkout 8190e061bc2d95da37479a638aa2c9e483e58ec6
    popd
else 
    echo "repo already exists, skipping clone"
fi


# we build our own corpus from all JSON files under the jsoncpp tree.
mkdir -p corpus/jsoncpp@fuzz
echo "FuzzBench does not ship a jsoncpp corpus; building one from all *.json files under jsoncpp."
# Filter out very small files to avoid skewing performance measurements.
find repo -type f -name "*.json" -size +512c -exec cp {} corpus/jsoncpp@fuzz/ \;

# For C++ programs, libc++ is not instrumented, so MSan may report false positives; filter inputs that trigger them.
FP_SEEDS=("pass1.json" "fail_test_stack_limit.json")
for seed in ${FP_SEEDS[@]}; do
    rm -f corpus/jsoncpp@fuzz/$seed
done

