#!/bin/bash
set -e  # Exit immediately if any command fails

export PROJ=strongswan

# Use the same overall workflow and variable conventions as tpm2-tss
# Enable shared libraries and core plugins to allow on-demand loading at runtime
CONFIG_OPTIONS="--enable-load-tester"
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

# Generate configure script
pushd "$REPO_DIR"
if [ ! -f "configure" ]; then
    if [ -x ./autogen.sh ]; then
        echo "Generating configure script..."
        ./autogen.sh
    else
        echo "Error: configure is missing and autogen.sh does not exist" >&2
        exit 1
    fi
fi
popd

pushd "$BUILD_DIR"
rm -rf *
rm -f config.cache
# Pre-determine per-mode output directories and use them as the build-time plugin directory (PLUGINDIR)
ARTEFACTS_DIR="$ROOT_DIR/artefacts/charon"
PLUGINS_DIR_MODE="$ARTEFACTS_DIR/plugins/$mode"
LIB_DIR_MODE="$ARTEFACTS_DIR/lib/$mode"
mkdir -p "$PLUGINS_DIR_MODE" "$LIB_DIR_MODE"

ac_cv_func_mallinfo2=no "$REPO_DIR"/configure $CONFIG_OPTIONS \
  --with-plugindir="$PLUGINS_DIR_MODE" \
  --cache-file=/dev/null
make clean || true
make -j

# Collect binaries into .bin inside the build directory
mkdir -p .bin

# Common strongSwan tools, excluding charon (it uses system-wide resources such as routing tables and special directories, which are hard to parallelize)
CANDIDATES=(
  swanctl
  pki
)

SEARCH_PATHS=(
  "./src/swanctl"
  "./src/pki"
  "../repo/src/swanctl"
  "../repo/src/pki"

)

is_elf() { file "$1" 2>/dev/null | grep -q "ELF"; }

found_any=0
for name in "${CANDIDATES[@]}"; do
  for dir in "${SEARCH_PATHS[@]}"; do
    if [ -f "$dir/$name" ]; then
      src="$dir/$name"
      # If this is charon, handle libtool wrappers and prefer the real ELF binary
      if [ "$name" = "charon" ]; then
        if [ -f "$dir/.libs/charon" ]; then
          src="$dir/.libs/charon"
        elif [ -f "$dir/.libs/lt-charon" ]; then
          src="$dir/.libs/lt-charon"
        fi
      fi
      cp "$src" ".bin/$name"
      # If it is still not ELF (may still be a wrapper script), fallback to executables under .libs
      if ! is_elf ".bin/$name" && [ -d "$dir/.libs" ]; then
        if [ -f "$dir/.libs/charon" ]; then
          cp "$dir/.libs/charon" ".bin/$name"
        elif [ -f "$dir/.libs/lt-charon" ]; then
          cp "$dir/.libs/lt-charon" ".bin/$name"
        fi
      fi
      found_any=1
      break
    fi
  done
done

# Ensure we do not keep any charon-related artefact directories
rm -rf ../artefacts/charon 2>/dev/null || true

if [ "$found_any" -eq 0 ]; then
  echo "Warning: strongSwan executables not found in expected paths." >&2
fi

# Fallback search: collect all executables under src
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find ./src ../repo/src -maxdepth 3 -type f -perm -111 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# Explicitly exclude charon (even if fallback search copied it into .bin)
rm -f .bin/charon 2>/dev/null || true

# Sync to artefacts: create artefacts/<name>/<name>.<mode> for each executable
for f in .bin/*; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  # Skip packaging of charon
  if [ "$name" = "charon" ]; then
    continue
  fi
  out_dir="../artefacts/$name"
  mkdir -p "$out_dir"
  cp -f "$f" "$out_dir/${name}.${mode}"
  chmod +x "$out_dir/${name}.${mode}" || true
  # For charon, additionally package plugins and shared libraries
  if [ "$name" = "charon" ]; then
    plug_out="$out_dir/plugins/$mode"
    lib_out="$out_dir/lib/$mode"
    mkdir -p "$plug_out" "$lib_out"
    # Collect plugin *.so files (including symlinks) and preserve attributes
    while IFS= read -r -d '' p; do
      cp -a "$p" "$plug_out/"
    done < <(find "$BUILD_DIR" -path "*/plugins/*" \( -type f -o -type l \) -name "*.so*" -print0 2>/dev/null || true)
    # Collect core shared libraries (such as libstrongswan / libcharon, including symlinks)
    while IFS= read -r -d '' s; do
      cp -a "$s" "$lib_out/"
    done < <(find "$BUILD_DIR" \( -type f -o -type l \) \( -name "libstrongswan.so*" -o -name "libcharon.so*" \) -print0 2>/dev/null || true)
    echo "[$mode] Packed $(ls -1 "$plug_out" 2>/dev/null | wc -l) plugins into $plug_out; shared libraries are placed in $lib_out" >&2
  fi
done

popd
