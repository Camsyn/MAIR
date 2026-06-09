#!/bin/bash
set -e

# Create a flat corpus for libpng@ossfuzz
CORPUS_DIR="corpus/libpng@ossfuzz"

echo "Creating libpng corpus..."
rm -rf "$CORPUS_DIR" && mkdir -p "$CORPUS_DIR"

# Following fuzzbench: collect a small number of PNGs from tests/samples as
# an initial corpus. Simplified: prefer repo/contrib/pngsuite, then tests.
COUNT=0
if [ -d "repo/contrib/pngsuite" ]; then
  find repo/contrib/pngsuite -type f -name "*.png" -exec cp {} "$CORPUS_DIR/" \; && COUNT=1
fi
if [ $COUNT -eq 0 ] && [ -d "repo/tests" ]; then
  find repo/tests -type f -name "*.png" -exec cp {} "$CORPUS_DIR/" \;
fi

echo "Number of corpus files: $(find "$CORPUS_DIR" -type f | wc -l)"

#!/bin/bash
set -e

# Create a more general test corpus for libpng

CORPUS_DIR="corpus"
REPO_DIR="repo"

echo "Creating libpng test corpus..."

# Create the corpus directory
mkdir -p "$CORPUS_DIR"

# Collect test files from the source tree
if [ -d "$REPO_DIR" ]; then
    # Find various types of test files
    find "$REPO_DIR" -name "*.zip" -o -name "*.png" -o -name "*.jpg" -o -name "*.jpeg" -o -name "*.xml" -o -name "*.json" -o -name "*.txt" -o -name "*.bin" -o -name "*.dat" | while read file; do
        if [[ "$file" == *"test"* ]] || [[ "$file" == *"sample"* ]] || [[ "$file" == *"seed"* ]] || [[ "$file" == *"corpus"* ]]; then
            echo "Copying test file: $file"
            cp "$file" "$CORPUS_DIR/"
        fi
    done
    
    # Unpack seed ZIP archives into the corpus directory
    for zip_file in "$CORPUS_DIR"/*.zip; do
        if [ -f "$zip_file" ]; then
            echo "Unpacking seed corpus: $zip_file"
            unzip -o "$zip_file" -d "$CORPUS_DIR/"
        fi
    done
fi

# If the corpus directory is still empty, create some basic test files
if [ ! "$(ls -A "$CORPUS_DIR")" ]; then
    echo "Creating basic test files..."
    echo "test" > "$CORPUS_DIR/test.txt"
    echo '{"test": "data"}' > "$CORPUS_DIR/test.json"
fi

echo "Corpus creation completed: $CORPUS_DIR"
echo "Number of corpus files: $(find "$CORPUS_DIR" -type f | wc -l)"
