#!/bin/bash
set -e

# Adapted from the fuzzbench version
mkdir -p repo corpus/sqlite3@ossfuzz


if [ ! -f sqlite3.tar.gz ]; then
    curl 'https://sqlite.org/src/tarball/sqlite.tar.gz?r=c78cbf2e86850cc6' -o sqlite3.tar.gz
fi

if ! file sqlite3.tar.gz | grep -q 'gzip compressed data'; then
    echo "Error: sqlite3.tar.gz is not a valid gzip archive; the download request was likely blocked by sqlite.org (for example due to a CAPTCHA or HTML page)." >&2
    echo "Please manually download sqlite3.tar.gz from the following link using a browser and place it under the repo directory:" >&2
    echo "    https://sqlite.org/src/tarball/sqlite.tar.gz?r=c78cbf2e86850cc6"
    exit 1
fi

pushd repo
tar xzf ../sqlite3.tar.gz --strip-components 1
popd


find repo -name "*.test" -exec cp {} corpus/sqlite3@ossfuzz/ \;
# Although sqlite3 owns a corpus set with 1000+ inputs,
# the most inputs fed to sqlite3@ossfuzz are too simple that the PUT execution time ~1ms, 
# which are not useful for our evaluation (the relevant biases are inevitable).
# Therefore, we only select seeds with PUT execution time >5ms or ~5ms.
WHITELIEST_SEEDS=(
    "crash01.test"
    "multiwrite01.test"
    "randexpr1.test"
    "window3.test"
    "fts1porter.test"
    "fts5porter.test"
)

# Move allowlisted seeds to tmp, remove the rest, then move tmp seeds back and remove tmp.
mkdir -p tmp_sqlite3_seeds
for w in "${WHITELIEST_SEEDS[@]}"; do
    if [ -f "corpus/sqlite3@ossfuzz/$w" ]; then
        mv "corpus/sqlite3@ossfuzz/$w" tmp_sqlite3_seeds/
    fi
done
rm -f corpus/sqlite3@ossfuzz/*.test
mv tmp_sqlite3_seeds/* corpus/sqlite3@ossfuzz/ 2>/dev/null || true
rmdir tmp_sqlite3_seeds
