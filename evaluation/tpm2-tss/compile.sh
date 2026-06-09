#!/bin/bash
set -e  # Exit immediately if any command fails

export PROJ=tpm2-tss

rm -rf temp && mkdir temp

CONFIG_OPTIONS="--disable-shared --disable-doc --disable-scripts --enable-unit --disable-integration"
export SUBJECT=$PWD
export LDFLAGS=-lpthread
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# Mode: prefer positional argument, then environment variable, default to raw
mode="${1:-${mode:-raw}}"

source ../activate_compile_flags.sh "$mode" false

# Generate the configure script first
pushd repo
if [ ! -f "configure" ]; then
    echo "Generating configure script..."
    ./bootstrap
fi
popd

pushd temp
rm -rf *
../repo/configure $CONFIG_OPTIONS
make clean
make -j

# Collect selected tpm2-tss executables into ../artefacts
mkdir -p .bin

# tpm2-tss mainly builds libraries, but also provides some tools
# Find executable files
find . -type f -executable -name "tss2*" -o -name "test*" | head -10

# Collect all matching executables
find . -type f -executable \( -name "tss2*" -o -name "test*" -o -name "*test*" \) -exec cp {} .bin/ \;

popd

# Collect executable files into temp/.bin
pushd temp
mkdir -p .bin

CANDIDATES=(tss2-test tss2-tcti-device tss2-tcti-mssim tss2-tcti-swtpm fapi-ima-fuzzing fapi-system-fuzzing)
SEARCH_PATHS=(
  "./src/tss2-tcti"
  "./src/tss2-sys"
  "./src/tss2-esys"
  "./test"
  "./test/unit"
  "../repo/src"
  "../repo/test"
)

found_any=0
for name in "${CANDIDATES[@]}"; do
  for dir in "${SEARCH_PATHS[@]}"; do
    if [ -f "$dir/$name" ]; then
      cp "$dir/$name" ".bin/$name"
      found_any=1
      break
    fi
  done
done

if [ "$found_any" -eq 0 ]; then
  echo "Warning: tpm2-tss executables not found in expected paths." >&2
fi

# Fallback search
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find ./test ./test/unit -maxdepth 2 -type f -perm -111 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# Sync into artefacts and generate files with mode suffix
mkdir -p ../artefacts/tpm2-tss
cp -f .bin/* ../artefacts/tpm2-tss/ 2>/dev/null || true

PRIMARY=""
for cand in tss2-test tss2-tcti-device tss2-tcti-mssim; do
  if [ -f "../artefacts/tpm2-tss/$cand" ]; then
    PRIMARY="$cand"; break
  fi
done

if [ -n "$PRIMARY" ]; then
  cp -f "../artefacts/tpm2-tss/$PRIMARY" "../artefacts/tpm2-tss/${PRIMARY}.${mode}"
  chmod +x "../artefacts/tpm2-tss/${PRIMARY}.${mode}" || true
fi

popd
