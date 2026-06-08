#!/bin/bash
export PROJ=binutils
URL="git://sourceware.org/git/binutils-gdb.git"


# Use depth 1 to avoid cloning the history
git clone "$URL" repo --depth 1

# 原仓库未提供输入语料。为便于冒烟与快速基准，这里生成基础语料：

# 目录就绪
mkdir -p ./corpus/cxxfilt \
         ./corpus/nm-new \
         ./corpus/objcopy \
         ./corpus/objdump \
         ./corpus/readelf \
         ./corpus/size \
         ./corpus/strings \
         ./corpus/strip-new

# 1) cxxfilt：用 nm 全量导出 7zip 二进制的符号列表，作为反修饰输入
if [ ! -s ./corpus/cxxfilt/7zz-nm-out.txt ]; then
  mkdir -p ./corpus/cxxfilt
  # 优先寻找 7zip artefact，可匹配不同模式产物；否则尝试系统 7z/7zz 可执行
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
    # 导出完整符号（不解码 C++ 名），取最后一列为符号名，过滤空行
    nm -a --no-demangle "$CANDIDATE_BIN" 2>/dev/null \
      | awk '{if (NF>0) print $NF}' \
      | sed 's/\r$//' \
      | grep -E '\S' \
      > ./corpus/cxxfilt/7zz-nm-out.txt || true
  fi

  # 兜底：若仍未生成，则写入一个最小样例，避免目录为空
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

# 2) 各工具的最小 ELF 原始输入：objdump.raw（如不存在则创建）
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

# 优先使用已存在的 artefacts/objdump/objdump.raw；否则回退到最小 ELF 生成
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

# 3) strings：若已有 XML/文本则保留；否则尝试从 libxml2 语料复制，仍无则造小样例
ensure_strings_min() {
  # 若目录已有文件，则不动作
  if [ -n "$(ls -A ./corpus/strings 2>/dev/null)" ]; then return; fi
  # 尝试从 libxml2 复制
  if [ -d ../libxml2/corpus/libxml2@ossfuzz ]; then
    find ../libxml2/corpus/libxml2@ossfuzz -maxdepth 1 -type f | head -n 20 | while read -r f; do
      cp -f "$f" ./corpus/strings/
    done
  fi
  # 仍为空则造 3 个小样
  if [ -z "$(ls -A ./corpus/strings 2>/dev/null)" ]; then
    cat > ./corpus/strings/sample1.txt << 'EOF'
Hello, binutils strings sample.
EOF
    cat > ./corpus/strings/min.xml << 'EOF'
<root><a>1</a><b>text</b></root>
EOF
    cat > ./corpus/strings/unicode.txt << 'EOF'
中文字符 / unicode ✓
EOF
  fi
}
ensure_strings_min
