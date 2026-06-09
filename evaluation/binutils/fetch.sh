#!/bin/bash
export PROJ=binutils
URL="https://sourceware.org/git/binutils-gdb.git"


# Use depth 1 to avoid cloning the history
git clone "$URL" repo --depth 1

# The original repository does not provide input corpora. Generate basic corpora for smoke tests and quick benchmarks.

# Ensure directories exist
mkdir -p ./corpus/cxxfilt \
         ./corpus/nm-new \
         ./corpus/objcopy \
         ./corpus/objdump \
         ./corpus/readelf \
         ./corpus/size \
         ./corpus/strings \
         ./corpus/strip-new

# 1) cxxfilt: export the full symbol list from the 7zip binary with nm as demangling input
if [ ! -s ./corpus/cxxfilt/7zz-nm-out.txt ]; then
  mkdir -p ./corpus/cxxfilt
  # Prefer 7zip artefacts to match different build modes; otherwise try system 7z/7zz binaries
  CANDIDATE_BIN=""
  if [ -d ../7zip/artefacts/7zz ]; then
    CANDIDATE_BIN=$(find ../7zip/artefacts/7zz -maxdepth 1 -type f -name '7zz.*' | head -n1)
  fi
  if [ -z "$CANDIDATE_BIN" ] && command -v 7zz >/dev/null 2>&1; then
    CANDIDATE_BIN=$(command -v 7zz)
  fi
  if [ -z "$CANDIDATE_BIN" ] && command -v 7z >/dev/null 2>&1; then
    CANDIDATE_BIN=$(command -v 7z)
  fi

  if [ -n "$CANDIDATE_BIN" ] && command -v nm >/dev/null 2>&1; then
    # Export all symbols without demangling C++ names, take the last field as the symbol name, and filter empty lines
    nm -a --no-demangle "$CANDIDATE_BIN" 2>/dev/null \
      | awk '{if (NF>0) print $NF}' \
      | sed 's/\r$//' \
      | grep -E '\S' \
      > ./corpus/cxxfilt/7zz-nm-out.txt || true
  fi

  # Fallback: if still empty, write a minimal sample so the directory is not empty
  if [ ! -s ./corpus/cxxfilt/7zz-nm-out.txt ]; then
    cat > ./corpus/cxxfilt/7zz-nm-out.txt << 'EOF'
__ZSt4cout
_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_
_ZNSt6vectorIiSaIiEE6push_backERKi
_ZNKSt8__detail20_Prime_rehash_policy14_M_need_rehashEmm
_ZN3foo3barEi
EOF
  fi
fi

# 2) Minimal ELF raw input for each tool: objdump.raw (create if missing)
gen_min_elf() {
  local dst="$1"
  if [ -s "$dst" ]; then return; fi
  mkdir -p "$(dirname "$dst")"
  cat > "${dst}.hex" << 'EOF'
7f454c46020101000000000000000000  \
01003e00010000000000000000000000  \
00000000000000000000000000000000  \
40003800000000004000000000000000
EOF
  xxd -r -p "${dst}.hex" > "$dst" 2>/dev/null || true
  rm -f "${dst}.hex" 2>/dev/null || true
}

# Prefer existing artefacts/objdump/objdump.raw; otherwise generate a minimal ELF file
ART_OBJDUMP_RAW=./artefacts/objdump/objdump.raw
if [ -f "$ART_OBJDUMP_RAW" ]; then
  for d in nm-new objcopy objdump readelf size strip-new; do
    mkdir -p "./corpus/${d}"
    cp -f "$ART_OBJDUMP_RAW" "./corpus/${d}/objdump.raw"
  done
else
  for d in nm-new objcopy objdump readelf size strip-new; do
    gen_min_elf "./corpus/${d}/objdump.raw"
  done
fi

# 3) strings: keep existing XML/text inputs; otherwise copy from the libxml2 corpus, then create small samples if still empty
ensure_strings_min() {
  # If the directory already has files, do nothing.
  if [ -n "$(ls -A ./corpus/strings 2>/dev/null)" ]; then return; fi
  # Try to copy from the libxml2 corpus
  if [ -d ../libxml2/corpus/libxml2@ossfuzz ]; then
    find ../libxml2/corpus/libxml2@ossfuzz -maxdepth 1 -type f | head -n 20 | while read -r f; do
      cp -f "$f" ./corpus/strings/
    done
  fi
  # If it is still empty, create three small sample files
  if [ -z "$(ls -A ./corpus/strings 2>/dev/null)" ]; then
    cat > ./corpus/strings/sample1.txt << 'EOF'
Hello, binutils strings sample.
EOF
    cat > ./corpus/strings/min.xml << 'EOF'
<root><a>1</a><b>text</b></root>
EOF
    cat > ./corpus/strings/unicode.txt << 'EOF'
Chinese characters / unicode ✓
EOF
  fi
}
ensure_strings_min

