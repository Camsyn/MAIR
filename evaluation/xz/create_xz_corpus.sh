#!/bin/bash
# Create xz test corpus:
# - Copy binutils/corpus/nm-new/objdump.raw as the original seed
# - Compress it with xz to obtain objdump.raw.xz

set -euo pipefail

SRC="../binutils/corpus/nm-new/objdump.raw"
DEST_DIR="corpus/xz"
SEED_RAW="${DEST_DIR}/objdump.raw"
SEED_XZ="${DEST_DIR}/objdump.raw.xz"
XZ_BIN="./artefacts/xz/xz.raw"

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

if [ ! -x "${XZ_BIN}" ]; then
  echo "ERROR: xz executable not found: ${XZ_BIN} (please build xz first)" >&2
  exit 1
fi

echo "Copying original seed..."
cp -f "${SRC}" "${SEED_RAW}"

echo "Creating compressed seed (.xz)..."
"${XZ_BIN}" -c "${SEED_RAW}" > "${SEED_XZ}"

echo "Corpus preparation done!"
ls -la "${DEST_DIR}"
echo "Number of files: $(ls "${DEST_DIR}" | wc -l)"
echo "Total size: $(du -sh "${DEST_DIR}")"






