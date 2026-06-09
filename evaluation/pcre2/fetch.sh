# #!/bin/bash
URL="https://github.com/PCRE2Project/pcre2.git"


# If repo/.git does not exist, clone and initialize submodules
if [ ! -d "repo/.git" ]; then
    # Use depth 1 to avoid cloning full history
    git clone "$URL" repo --depth 1

    pushd repo
    git submodule update --init
    popd
fi

mkdir -p corpus/pcre2test
mkdir -p corpus/pcre2grep


# Copy repository test data into the pcre corpus
cp repo/testdata/testinput* corpus/pcre2test
dd if=/dev/zero bs=1 count=1M | tr '\0' '\x61' > corpus/pcre2grep/test.txt

