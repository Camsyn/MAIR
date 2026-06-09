#!/bin/bash
set -e

CORPUS_DIR="corpus/lcms@cms_transform_fuzzer"

echo "Creating test corpus for lcms@ossfuzz..."

rm -rf "$CORPUS_DIR" && mkdir -p "$CORPUS_DIR"

# Prefer to use seeds from fuzzbench
if [ -d "fuzzbench-temp/benchmarks/lcms_cms_transform_fuzzer/seeds" ]; then
    cp -r fuzzbench-temp/benchmarks/lcms_cms_transform_fuzzer/seeds/* "$CORPUS_DIR/" || true
    echo "Copied fuzzbench seed files into $CORPUS_DIR"
fi

# Copy dictionary files from oss-fuzz
if [ -f "oss-fuzz-temp/projects/lcms/icc.dict" ]; then
    cp oss-fuzz-temp/projects/lcms/icc.dict "$CORPUS_DIR/"
    echo "Copied ICC dictionary file into $CORPUS_DIR"
fi

# Extract additional ICC test files from the local repo
if [ -d "repo" ]; then
    # ICC/ICM profile files and binary samples
    find repo -type f \( -iname "*.icc" -o -iname "*.icm" -o -iname "*.bin" \) -exec cp {} "$CORPUS_DIR/" \; || true
    echo "Copied ICC test files from repo into $CORPUS_DIR"
fi

# Fallback: if there are still no seeds, create some basic ICC files
if [ "$(find "$CORPUS_DIR" -type f | wc -l)" -eq 0 ]; then
    echo "Creating basic ICC seed files..."
    # Create a minimal ICC file header
    cat > "$CORPUS_DIR/minimal.icc" << 'EOF'
ICC_PROFILE
EOF
    echo "Basic seed files created"
fi

echo "Corpus creation completed: $CORPUS_DIR"
echo "Number of corpus files: $(find "$CORPUS_DIR" -type f | wc -l)"


