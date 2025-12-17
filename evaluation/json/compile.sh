#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

export PROJ=pcre2


rm -rf temp && mkdir temp

source ../activate_compile_flags.sh $mode true


# 这是一个 C++的 头文件库项目，因此编译方式比较特殊
# 该目录包含 json 的 fuzz 接口
mkdir -p temp/.bin
pushd repo/tests

# 1. 我们仅仅对项目本体进行插桩，而不对 libc++.so 进行插桩。
# 2. parse_afl_fuzzer 和 parse_bson_fuzzer 会调用 std::to_string 这一个定义在 string.cpp@libc++ 里的符号
# 3. 因此，MSan 会由于认为 std::to_string 返回的 std::string 未初始化而产生误报
# 4. 因此，而MSan报告时会严重增加 overhead，出于公平考虑，我们将 parse_afl_fuzzer parse_bson_fuzzer 排除
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

# 先编译每个fuzzer的object文件到../../temp目录，再分别链接生成可执行文件
# 并行编译各个目标，等待全部完成后再继续

# 1. 并行编译 object 文件
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
# 等待所有 object 编译完成
for job in "${compile_jobs[@]}"; do
    wait $job
done

# 2. 并行链接生成可执行文件
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
# 等待所有链接过程完成
for job in "${link_jobs[@]}"; do
    wait $job
done

for bin in "${BINARIES[@]}"; do
    if [ -f "$bin" ]; then
        mv "$bin" "../../temp/.bin/json@$bin"
    fi
done

popd

