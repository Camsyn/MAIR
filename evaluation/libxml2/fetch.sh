#!/bin/bash
set -e

# Create required directories
mkdir -p repo corpus/libxml2@ossfuzz

# Clone source code from the official libxml2 repository
if [ ! -d "repo/.git" ]; then
    git clone https://gitlab.gnome.org/GNOME/libxml2.git repo
else
    pushd repo
    git pull origin master
    popd
fi

# Fetch fuzzer files from oss-fuzz
echo "Fetching fuzzer files from oss-fuzz..."
if [ ! -d "oss-fuzz-temp" ]; then
    git clone https://github.com/google/oss-fuzz.git oss-fuzz-temp --depth 1
else
    pushd oss-fuzz-temp
    git pull origin master || git pull origin main
    popd
fi

# Fetch fuzzer files from fuzzbench
echo "Fetching fuzzer files from fuzzbench..."
if [ ! -d "fuzzbench-temp" ]; then
    git clone https://github.com/google/fuzzbench.git fuzzbench-temp --depth 1
else
    pushd fuzzbench-temp
    git pull origin master || git pull origin main
    popd
fi

# Check whether libxml2 exists as a project in oss-fuzz
echo "Checking libxml2 project in oss-fuzz..."
if [ -d "oss-fuzz-temp/projects" ]; then
    ls oss-fuzz-temp/projects/ | grep -i xml || echo "No XML-related projects found"
fi

# Copy fuzzer source files
if [ -f "oss-fuzz-temp/projects/libxml2/xml_read_memory_fuzzer.c" ]; then
    cp oss-fuzz-temp/projects/libxml2/xml_read_memory_fuzzer.c repo/
    echo "Copied xml_read_memory_fuzzer.c"
else
    echo "Warning: xml_read_memory_fuzzer.c not found"
fi

if [ -f "oss-fuzz-temp/projects/libxml2/xml_read_memory_fuzzer.h" ]; then
    cp oss-fuzz-temp/projects/libxml2/xml_read_memory_fuzzer.h repo/
    echo "Copied xml_read_memory_fuzzer.h"
else
    echo "Warning: xml_read_memory_fuzzer.h not found"
fi

# Skip autogen.sh and use the pre-generated configure script directly
echo "Skipping autogen.sh, using pre-generated configure script..."

echo "fetch finished."


# Search the repository to build an initial corpus
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_libxml2_corpus.sh" ]; then
    "$SCRIPT_DIR/create_libxml2_corpus.sh" || true
else
    echo "Warning: Executable create_libxml2_corpus.sh not found, skipping corpus generation" >&2
fi

rm -rf oss-fuzz-temp fuzzbench-temp