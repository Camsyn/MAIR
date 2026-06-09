#!/bin/bash
set -e

export PROJ=zstd

rm -rf temp && mkdir temp

export SUBJECT=$PWD

# Mode: prefer positional argument, then environment variable, default to raw
mode="${1:-${mode:-raw}}"

# Minimal environment settings consistent with binutils (do not forcibly switch compiler/linker)
export LDFLAGS=-lpthread
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# Import sanitizer flags: zstd is a C project, so pass false here
source ../activate_compile_flags.sh "$mode" false

# Backup and isolate flags to avoid being overwritten by make variables
export XSAN_CFLAGS="$CFLAGS"
export XSAN_CXXFLAGS="$CXXFLAGS"
export XSAN_LDFLAGS="$LDFLAGS"
unset CFLAGS CXXFLAGS LDFLAGS

# Generate compiler wrappers to inject CFLAGS/CXXFLAGS/LDFLAGS (without modifying upstream Makefiles)
TOOLCHAIN_DIR="$SUBJECT/temp/.toolchain"
mkdir -p "$TOOLCHAIN_DIR"

# Choose the real compiler: use xclang by default if available; set FORCE_GCC=1 to force GCC
if [ "${FORCE_GCC:-0}" = "1" ]; then
  REAL_CC="$(command -v xgcc)"
  REAL_CXX="$(command -v xg++)"
elif command -v xclang >/dev/null 2>&1 && command -v xclang++ >/dev/null 2>&1; then
  REAL_CC="$(command -v xclang)"
  REAL_CXX="$(command -v xclang++)"
else
  REAL_CC="$(command -v xgcc)"
  REAL_CXX="$(command -v xg++)"
fi

cat >"$TOOLCHAIN_DIR/cc-wrap" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# Distinguish pure C compilation from potential linking
is_compile=0
for a in "$@"; do
  case "$a" in
    -c|-S|-E) is_compile=1; break ;;
  esac
done
if [ "$is_compile" -eq 1 ]; then
  exec "${REAL_CC}" ${XSAN_CFLAGS:-} "$@"
else
  exec "${REAL_CC}" "$@" ${XSAN_LDFLAGS:-}
fi
EOF
cat >"$TOOLCHAIN_DIR/cxx-wrap" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# Determine whether this is a compilation phase (presence of -c/-S/-E)
is_compile=0
for a in "$@"; do
  case "$a" in
    -c|-S|-E) is_compile=1; break ;;
  esac
done
if [ "$is_compile" -eq 1 ]; then
  exec "${REAL_CXX}" ${XSAN_CXXFLAGS:-} "$@"
else
  # Treat as link phase: do not pass CXXFLAGS, use LDFLAGS instead
  exec "${REAL_CXX}" "$@" ${XSAN_LDFLAGS:-}
fi
EOF
cat >"$TOOLCHAIN_DIR/link-wrap" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# Filter compile-only options to avoid treating linking as compilation
filtered=()
for a in "$@"; do
  case "$a" in
    -c|-S|-E) continue ;;
    *) filtered+=("$a") ;;
  esac
done
exec "${REAL_CXX}" "${filtered[@]}" ${XSAN_LDFLAGS:-}
EOF
chmod +x "$TOOLCHAIN_DIR/cc-wrap" "$TOOLCHAIN_DIR/cxx-wrap" "$TOOLCHAIN_DIR/link-wrap"

export REAL_CC
export REAL_CXX
export CC="$TOOLCHAIN_DIR/cc-wrap"
export CXX="$TOOLCHAIN_DIR/cxx-wrap"
export LINK="$TOOLCHAIN_DIR/link-wrap"
export AR="$(command -v ar)"
export RANLIB="$(command -v ranlib)"
export STRIP="$(command -v strip)"
export OBJCOPY="$(command -v objcopy)"


cp -r repo/* temp/

pushd temp

# Follow binutils style: clean first, then parallel build
make clean || true
make -j CC="$CC" CXX="$CXX" PREFIX="$SUBJECT/temp/_o"

# Collect executables into temp/.bin
mkdir -p .bin

# Main zstd executables
CANDIDATES=(zstd)
SEARCH_PATHS=(
  "./_o/bin"
  "./_o"
  "./"
  "./programs"
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
  echo "Warning: zstd executable not found in expected paths." >&2
fi

# Fallback search
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find ../repo -maxdepth 6 -type f -perm -111 -name zstd 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# Sync into artefacts and generate files with mode suffix
mkdir -p ../artefacts/zstd
cp -f .bin/* ../artefacts/zstd/ 2>/dev/null || true

PRIMARY=""
for cand in zstd; do
  if [ -f "../artefacts/zstd/$cand" ]; then
    PRIMARY="$cand"; break
  fi
done

if [ -n "$PRIMARY" ]; then
  cp -f "../artefacts/zstd/$PRIMARY" "../artefacts/zstd/${PRIMARY}.${mode}"
  chmod +x "../artefacts/zstd/${PRIMARY}.${mode}" || true
fi

popd






