#!/bin/bash
set -e

# Based on the sqlite3 template
mkdir -p repo corpus/re2@ossfuzz

# Fetch re2 source code
if [ ! -d repo/.git ]; then
  git clone https://github.com/google/re2.git repo --depth 1
else
  (cd repo && git pull --ff-only)
fi

# Fetch fuzzer source (re2_fuzzer.cc) from oss-fuzz
if [ ! -d oss-fuzz-temp ]; then
  git clone https://github.com/google/oss-fuzz.git oss-fuzz-temp --depth 1
else
  (cd oss-fuzz-temp && git pull --ff-only origin master || git pull --ff-only origin main || true)
fi

if [ -f oss-fuzz-temp/projects/re2/re2_fuzzer.cc ]; then
  cp -f oss-fuzz-temp/projects/re2/re2_fuzzer.cc repo/
fi

# Optionally copy seeds (if they exist)
if [ -d oss-fuzz-temp/projects/re2/seeds ]; then
  cp -rf oss-fuzz-temp/projects/re2/seeds/* corpus/re2@re2_fuzzer/ || true
fi

# Apply MSan suppression patch if present
if [ -f repo.patch ]; then
  echo "Applying MSan suppression patch..."
  (cd repo && git apply ../repo.patch)
fi

# Search the repository for corpus files
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_re2_corpus.sh" ]; then
    "$SCRIPT_DIR/create_re2_corpus.sh" || true
else
    echo "Warning: Executable create_re2_corpus.sh not found, skipping corpus generation" >&2
fi  

rm -rf oss-fuzz-temp fuzzbench-temp

