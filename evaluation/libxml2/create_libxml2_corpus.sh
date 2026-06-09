#!/bin/bash
set -e

CORPUS_DIR="corpus/libxml2@xml_fuzzer"

echo "Creating test corpus for libxml2@ossfuzz..."

# Clean and create the corpus directory
rm -rf "$CORPUS_DIR" && mkdir -p "$CORPUS_DIR"

# Copy seed files from fuzzbench
if [ -d "fuzzbench-temp/benchmarks/libxml2_xml_read_memory_fuzzer/seeds" ]; then
    cp -r fuzzbench-temp/benchmarks/libxml2_xml_read_memory_fuzzer/seeds/* "$CORPUS_DIR/" 2>/dev/null || true
    echo "Copied fuzzbench seeds into $CORPUS_DIR"
fi

# Copy seed files from oss-fuzz
if [ -d "oss-fuzz-temp/projects/libxml2/corpus" ]; then
    cp -r oss-fuzz-temp/projects/libxml2/corpus/* "$CORPUS_DIR/" 2>/dev/null || true
    echo "Copied oss-fuzz seeds into $CORPUS_DIR"
fi

# Extract XML test files from the local repo
if [ -d "repo" ]; then
    # Find XML test files
    find repo -type f \( -name "*.xml" -o -name "*.xhtml" -o -name "*.xsd" -o -name "*.xsl" \) -exec cp {} "$CORPUS_DIR/" \; 2>/dev/null || true
    echo "Copied XML test files from repo into $CORPUS_DIR"
    
    # Find files under the test directory
    if [ -d "repo/test" ]; then
        find repo/test -type f -name "*.xml" -exec cp {} "$CORPUS_DIR/" \; 2>/dev/null || true
        echo "Copied XML files from repo/test into $CORPUS_DIR"
    fi
fi

# If there are still no seeds, create some basic XML files
if [ "$(find "$CORPUS_DIR" -type f | wc -l)" -eq 0 ]; then
    echo "Creating basic XML seed files..."
    
    # Create a minimal XML file
    cat > "$CORPUS_DIR/minimal.xml" << 'EOF'
<?xml version="1.0"?>
<root>
    <item>test</item>
</root>
EOF

    # Create an XML file that contains special characters
    cat > "$CORPUS_DIR/special_chars.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<root>
    <item attr="&lt;&gt;&amp;&quot;&apos;">&lt;&gt;&amp;&quot;&apos;</item>
</root>
EOF

    # Create an XML file that contains CDATA
    cat > "$CORPUS_DIR/cdata.xml" << 'EOF'
<?xml version="1.0"?>
<root>
    <![CDATA[This is CDATA content with <tags> and & symbols]]>
</root>
EOF

    echo "Basic seed files created"
fi

# Only keep the largest 30 seeds by file size
TOTAL_FILES=$(find "$CORPUS_DIR" -type f | wc -l | tr -d ' ')
if [ "$TOTAL_FILES" -gt 30 ]; then
    echo "Total number of seeds: $TOTAL_FILES, keeping the largest 30 files..."
    TMP_DIR=$(mktemp -d)
    # Collect paths for the top 30 largest files (sorted by size desc)
    mapfile -t TOP30 < <(find "$CORPUS_DIR" -type f -printf '%s\t%p\n' | sort -nr | head -n 30 | cut -f2-)
    # Copy into a temporary directory and then swap back so that only these 30 remain
    for f in "${TOP30[@]}"; do
        cp -p "$f" "$TMP_DIR/"
    done
    rm -rf "$CORPUS_DIR"
    mkdir -p "$CORPUS_DIR"
    cp -p "$TMP_DIR"/* "$CORPUS_DIR/" 2>/dev/null || true
    rm -rf "$TMP_DIR"
    echo "Top 30 retained files:"
    find "$CORPUS_DIR" -maxdepth 1 -type f -printf ' - %p (%s bytes)\n' | sort -nr -k2 | head -n 30
fi

echo "Corpus creation completed: $CORPUS_DIR"
echo "Number of corpus files: $(find "$CORPUS_DIR" -type f | wc -l)"