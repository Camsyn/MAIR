#!/bin/bash
set -e  # Exit immediately if any command fails

rm -rf temp && mkdir temp

source ../activate_compile_flags.sh $mode true

mkdir -p temp/.bin

build_dir=$PWD/temp/build

cp -r repo/* temp/
pushd temp
./Configure --prefix=$build_dir --openssldir=$build_dir '-Wl,-rpath,$(LIBRPATH)' -no-docs -no-asm -no-shared
make -j
make install
popd

cp temp/build/bin/openssl temp/.bin/openssl
