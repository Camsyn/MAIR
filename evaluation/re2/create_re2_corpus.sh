#!/bin/bash
set -e

CORPUS_DIR="corpus/re2@re2_fuzzer"

echo "Creating test corpus for re2@ossfuzz..."

# Clean and create the corpus directory
rm -rf "$CORPUS_DIR" && mkdir -p "$CORPUS_DIR"

# Pull fuzzer seeds from the oss-fuzz git repository
echo "Fetching fuzzer seeds from oss-fuzz git..."
if [ ! -d "oss-fuzz-temp" ]; then
    git clone https://github.com/google/oss-fuzz.git oss-fuzz-temp --depth 1
else
    pushd oss-fuzz-temp
    git pull origin master
    popd
fi

# Copy seed files into the corpus directory
if [ -d "oss-fuzz-temp/projects/re2/corpus" ]; then
    cp -r "oss-fuzz-temp/projects/re2/corpus"/* "$CORPUS_DIR/"
    echo "Copied seed files into $CORPUS_DIR"
elif [ -d "fuzzbench-temp/benchmarks/re2_fuzzer/seeds" ]; then
    cp -r "fuzzbench-temp/benchmarks/re2_fuzzer/seeds"/* "$CORPUS_DIR/"
    echo "Copied fuzzbench seed files into $CORPUS_DIR"
else
    echo "Fetching regular expression seeds from fuzzing dictionaries..."
    # Fetch regex seeds from Google's fuzzing dictionaries
    curl -s https://raw.githubusercontent.com/google/fuzzing/master/dictionaries/regexp.dict > "$CORPUS_DIR/regexp_dict.txt"
    echo "Downloaded regular expression dictionary to $CORPUS_DIR/regexp_dict.txt"
    
    echo "Extracting test files from the source repository..."
    # Extract test files from the RE2 source repository
    if [ -d "repo" ]; then
        echo "Extracting test files from the repo directory..."
        find repo -name "*.txt" -exec cp {} "$CORPUS_DIR/" \;
        find repo -name "*.test" -exec cp {} "$CORPUS_DIR/" \;
        find repo -name "*test*" -type f -name "*.cc" -exec head -20 {} \; > "$CORPUS_DIR/test_patterns.txt" || true
        find repo -name "*example*" -type f -name "*.cc" -exec head -20 {} \; > "$CORPUS_DIR/example_patterns.txt" || true
    fi
    
    # If there are still no seeds, create some basic ones
    if [ "$(find "$CORPUS_DIR" -type f | wc -l)" -eq 0 ]; then
        echo "Creating basic regular expression seeds..."
        cat > "$CORPUS_DIR/simple.txt" <<EOF
abc
EOF
        cat > "$CORPUS_DIR/alternation.txt" <<EOF
(a|b|c)+
EOF
        cat > "$CORPUS_DIR/quantifiers.txt" <<EOF
^\d{3,5}[a-z]*$
EOF
        cat > "$CORPUS_DIR/character_classes.txt" <<EOF
[0-9]+[a-zA-Z]*
EOF
        cat > "$CORPUS_DIR/anchors.txt" <<EOF
^start.*end$
EOF
        cat > "$CORPUS_DIR/complex.txt" <<EOF
^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$
EOF
    fi
fi

# Clean up temporary directories
rm -rf oss-fuzz-temp

echo "Corpus created: $CORPUS_DIR"
echo "Number of corpus files: $(find "$CORPUS_DIR" -type f | wc -l)"
