#!/bin/bash
set -e

export PROJ="curl"
URL="https://github.com/curl/curl.git"

echo "Cloning curl source..."
if [ ! -d "repo" ]; then
    git clone $URL repo --depth 1
else
    pushd repo
    git pull origin master
    popd
fi

echo "Fetching fuzzer code and seeds from curl-fuzzer..."
if [ ! -d "curl-fuzzer-temp" ]; then
    git clone https://github.com/curl/curl-fuzzer.git curl-fuzzer-temp --depth 1
else
    pushd curl-fuzzer-temp
    git pull origin master
    popd
fi

# Copy curl_fuzzer.cc and related files into repo/tests/fuzzer/
mkdir -p repo/tests/fuzzer
cp "curl-fuzzer-temp/curl_fuzzer.cc" repo/tests/fuzzer/
cp "curl-fuzzer-temp/curl_fuzzer.h" repo/tests/fuzzer/
cp "curl-fuzzer-temp/curl_fuzzer_callback.cc" repo/tests/fuzzer/
cp "curl-fuzzer-temp/curl_fuzzer_tlv.cc" repo/tests/fuzzer/
cp "curl-fuzzer-temp/testinput.h" repo/tests/fuzzer/

# Copy corpus files from the curl-fuzzer repository into corpus/curl@curl_fuzzer
SRC_ROOT="curl-fuzzer-temp/corpora"
echo "Selecting the 30 largest corpus files from corpora..."
mkdir -p corpus/curl@curl_fuzzer
if [ -d "$SRC_ROOT" ]; then
  TOTAL=$(find "$SRC_ROOT" -type f 2>/dev/null | wc -l)
  echo "Total number of source corpus files: $TOTAL"
  SEL=0
  TMP_LIST=$(mktemp)
  # Build a (size \t path) list
  find "$SRC_ROOT" -type f -printf '%s\t%p\n' 2>/dev/null > "$TMP_LIST" || true
  # Select the 30 largest seeds and copy them one by one to avoid pipeline subshell scope issues
  while IFS= read -r __path; do
    [ -f "$__path" ] || continue
    cp -f -- "$__path" corpus/curl@curl_fuzzer/ 2>/dev/null || true
    SEL=$((SEL+1))
  done < <(LC_ALL=C sort -nr "$TMP_LIST" | head -n 30 | cut -f2-)
  rm -f "$TMP_LIST" || true
  echo "Copied $SEL files into corpus/curl@curl_fuzzer"
else
  echo "Warning: $SRC_ROOT directory not found, skipping corpus copy."
fi

echo "fetch finished. Number of corpus files: $(find corpus/curl@curl_fuzzer -type f 2>/dev/null | wc -l)"

# Keep curl-fuzzer-temp for inspection/reruns; clean only unrelated directories
rm -rf oss-fuzz-temp curl-fuzzer-temp
