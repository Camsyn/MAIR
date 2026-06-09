#!/bin/bash
set -e

# Create a test corpus for libjpeg-turbo@libjpeg_turbo_fuzzer by directly
# extracting sample inputs into the corpus directory (no compression).

CORPUS_DIR="corpus/libjpeg-turbo@libjpeg_turbo_fuzzer"
REPO_DIR="repo"

echo "Creating test corpus for libjpeg-turbo@libjpeg_turbo_fuzzer..."

# Clean and create the corpus directory
rm -rf "$CORPUS_DIR" && mkdir -p "$CORPUS_DIR"

# If a corpus already exists, skip regeneration to avoid duplication.
if [ "$(find "$CORPUS_DIR" -type f | wc -l)" -gt 0 ]; then
    echo "Corpus already exists, skipping generation."
    echo "Number of corpus files: $(find "$CORPUS_DIR" -type f | wc -l)"
    exit 0
fi

# Save the original working directory
ORIGINAL_DIR="$PWD"

# Create a temporary directory
TEMP_DIR=$(mktemp -d)
cd "$TEMP_DIR"

# Clone the seed-corpora repository
echo "Cloning libjpeg-turbo/seed-corpora repository..."
git clone https://github.com/libjpeg-turbo/seed-corpora
git -C seed-corpora checkout 7c9ea5ffaac76ef618657978c9fdfa845d310b93

# Directly copy test images into the corpus directory
echo "Extracting test images into the corpus directory..."
cd seed-corpora

# # Copy AFL test cases
# if [ -d "afl-testcases" ]; then
#     find afl-testcases -path "*/jpeg*" -type f -exec cp {} "$ORIGINAL_DIR/$CORPUS_DIR/" \;
# fi

# # Copy decompression test cases from bug reports
# if [ -d "bugs" ]; then
#     find bugs -path "*/decompress*" -type f -exec cp {} "$ORIGINAL_DIR/$CORPUS_DIR/" \;
# fi

# Copy test JPEG images from the repo's testimages directory
if [ -d "$ORIGINAL_DIR/$REPO_DIR/testimages" ]; then
    find "$ORIGINAL_DIR/$REPO_DIR/testimages" -name "*.jpg" -type f -exec cp {} "$ORIGINAL_DIR/$CORPUS_DIR/" \;
fi

# Clean up the temporary directory
cd "$ORIGINAL_DIR"
rm -rf "$TEMP_DIR"

echo "Corpus creation completed: $CORPUS_DIR"
echo "Number of corpus files: $(find "$CORPUS_DIR" -type f | wc -l)"