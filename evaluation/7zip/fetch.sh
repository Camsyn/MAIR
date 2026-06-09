#!/bin/bash
export PROJ=7zip
URL="https://github.com/mcmilk/7-Zip.git"


# Use depth 1 to avoid cloning the history
git clone "$URL" repo --depth 1

# The upstream repo provides no input; as this is a compression/decompression tool, we provide corpus/7zz/sample1.txt.
mkdir -p ./corpus/7zz
cat > ./corpus/7zz/sample1.txt << 'EOF'
This is a small sample text file for 7zip benchmarking.
It is intentionally short to keep smoke tests fast and deterministic.
EOF
