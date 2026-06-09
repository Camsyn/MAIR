#!/bin/bash
set -e  # Exit immediately if any command fails

export PROJ=iproute2

# Use the same overall workflow and variable conventions as tpm2-tss
export SUBJECT=$PWD
export LDFLAGS="${LDFLAGS:+$LDFLAGS }-lpthread"
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# Mode: prefer positional argument, then environment variable, default to raw
mode="${1:-${mode:-raw}}"

# Activate compile flags (C project, second parameter false)
source ../activate_compile_flags.sh "$mode" false

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR="$ROOT_DIR/repo"
BUILD_DIR="$ROOT_DIR/temp"

rm -rf "$BUILD_DIR" && mkdir "$BUILD_DIR"

# Copy source code into the build directory
cp -r "$REPO_DIR"/* "$BUILD_DIR/"

pushd "$BUILD_DIR"

# Generate config.mk
if [ ! -f "config.mk" ]; then
    echo "Generating config.mk..."
    ./configure
fi

# Clean previous builds
make clean || true

# Build
make -j

# Collect binaries into .bin within the build directory
mkdir -p .bin

# Main iproute2 tools
CANDIDATES=(
  ip
  tc
  ss
  bridge
  devlink
  genl
  rdma
  tipc
  vdpa
  arpd
  nstat
  rtacct
  lnstat
  ifstat
)

SEARCH_PATHS=(
  "./ip"
  "./tc"
  "./misc"
  "./bridge"
  "./devlink"
  "./genl"
  "./rdma"
  "./tipc"
  "./vdpa"
  "./misc/arpd"
  "./misc/nstat"
  "./misc/rtacct"
  "./misc/lnstat"
  "./misc/rtstat"
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
  echo "Warning: iproute2 executables not found in expected paths." >&2
fi

# Fallback search: grab all executables under the current directory
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find . -maxdepth 2 -type f -perm -111 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# Sync into artefacts: generate <name>.<mode> for each executable
for f in .bin/*; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  out_dir="../artefacts/$name"
  mkdir -p "$out_dir"
  cp -f "$f" "$out_dir/${name}.${mode}"
  chmod +x "$out_dir/${name}.${mode}" || true
done

popd

echo "Done: iproute2($mode) build and artefact collection."
