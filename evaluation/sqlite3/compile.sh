#!/bin/bash
set -e  # Exit immediately if any command fails

rm -rf temp && mkdir temp

#!/bin/bash
export ASAN_OPTIONS=detect_leaks=0

# Limit max length of data blobs and sql queries to prevent irrelevant OOMs.
# Also limit max memory page count to avoid creating large databases.
export CFLAGS="$CFLAGS -DSQLITE_MAX_LENGTH=128000000 \
               -DSQLITE_MAX_SQL_LENGTH=128000000 \
               -DSQLITE_MAX_MEMORY=25000000 \
               -DSQLITE_PRINTF_PRECISION_LIMIT=1048576 \
               -DSQLITE_DEBUG=1 \
               -DSQLITE_MAX_PAGE_COUNT=16384"  

source ../activate_compile_flags.sh "$mode" true # is_cxx = true, as the driver is C++

pushd temp

SRC_DIR=../repo
mkdir -p .bin

$SRC_DIR/configure --disable-shared
make -j
make sqlite3.c

$CC $CFLAGS -I. -c $SRC_DIR/test/ossfuzz.c -o ./ossfuzz.o

$CXX $CXXFLAGS ./ossfuzz.o -o .bin/sqlite3@ossfuzz \
    $LIB_FUZZING_ENGINE ./sqlite3.o -pthread -ldl -lz
popd 
