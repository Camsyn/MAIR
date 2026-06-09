#!/bin/bash
# Create zstd test corpus:
# - Copy binutils/corpus/nm-new/objdump.raw as the original seed
# - Compress it with zstd to obtain objdump.raw.zstd

set -euo pipefail

SRC="../binutils/corpus/nm-new/objdump.raw"
DEST_DIR="corpus/zstd"
SEED_RAW="${DEST_DIR}/objdump.raw"
SEED_7Z="${DEST_DIR}/objdump.raw.zstd"
ZSTD_BIN="./artefacts/zstd/zstd.raw"

# First clean the destination directory
if [ -d "${DEST_DIR}" ]; then
  rm -rf "${DEST_DIR:?}"/*
else
  mkdir -p "${DEST_DIR}"
fi

# Ensure the directory exists
mkdir -p "${DEST_DIR}"

if [ ! -f "${SRC}" ]; then
  echo "ERROR: Source file not found: ${SRC}" >&2
  exit 1
fi

if [ ! -x "${ZSTD_BIN}" ]; then
  echo "ERROR: zstd executable not found: ${ZSTD_BIN} (please build zstd first)" >&2
  exit 1
fi

echo "Copying original seed..."
cp -f "${SRC}" "${SEED_RAW}"

echo "Creating compressed seed (.zstd)..."
"${ZSTD_BIN}" -q -f -o "${SEED_7Z}" -- "${SEED_RAW}"

echo "Corpus preparation done!"
ls -la "${DEST_DIR}"
echo "Number of files: $(ls "${DEST_DIR}" | wc -l)"






