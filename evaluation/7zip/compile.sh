#!/bin/bash
set -e

export PROJ=7-Zip
# To let 7zz reserve its symbols
export DEBUG_BUILD=1

rm -rf temp && mkdir temp

export SUBJECT=$PWD

# 模式：优先位置参数，其次环境变量，默认 raw
mode="${1:-${mode:-raw}}"

# 与 binutils 一致的最小环境设置（不强行切换编译器/链接器）
export LDFLAGS=-lpthread
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# 导入外部消毒器旗标：7zip 为 C++ 项目，这里应当传 true 以启用 -stdlib=libc++ 等
source ../activate_compile_flags.sh "$mode" true

# 备份并隔离旗标，避免 make 自身变量覆盖
export XSAN_CFLAGS="$CFLAGS"
export XSAN_CXXFLAGS="$CXXFLAGS"
export XSAN_LDFLAGS="$LDFLAGS"
unset CFLAGS CXXFLAGS LDFLAGS

# 生成编译器包装器，注入 CFLAGS/CXXFLAGS/LDFLAGS（不修改上游 makefile）
TOOLCHAIN_DIR="$SUBJECT/temp/.toolchain"
mkdir -p "$TOOLCHAIN_DIR"

# 选择真实编译器：默认使用 xclang（若存在）；如需强制 GCC，设置 FORCE_GCC=1
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
# 纯 C 编译与可能的链接区分
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
# 判断是否为编译阶段（存在 -c/-S/-E）
is_compile=0
for a in "$@"; do
  case "$a" in
    -c|-S|-E) is_compile=1; break ;;
  esac
done
if [ "$is_compile" -eq 1 ]; then
  exec "${REAL_CXX}" ${XSAN_CXXFLAGS:-} "$@"
else
  # 认为是链接阶段：不要带 CXXFLAGS，使用 LDFLAGS
  exec "${REAL_CXX}" "$@" ${XSAN_LDFLAGS:-}
fi
EOF
cat >"$TOOLCHAIN_DIR/link-wrap" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# 过滤编译专用开关，避免把链接当成编译
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

pushd repo/CPP/7zip/Bundles/Alone2

# 与 binutils 风格一致：先 clean 再并行构建
make -rR -f makefile.gcc O="$SUBJECT/temp/_o" CC="$CC" CXX="$CXX" LINK="$LINK" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" OBJCOPY="$OBJCOPY" clean || true
make -rR -f makefile.gcc O="$SUBJECT/temp/_o" CC="$CC" CXX="$CXX" LINK="$LINK" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" OBJCOPY="$OBJCOPY" -j

# 上游 make 已通过 O= 重定向到顶层 temp/_o/
OUTDIR="$SUBJECT/temp/_o"

popd

# 收集可执行文件到 temp/.bin
pushd temp
mkdir -p .bin

CANDIDATES=(7zz 7za 7zr 7z)
SEARCH_PATHS=(
  "./_o"
  "../repo/CPP/7zip/Bundles/Alone2/_o"
  "../repo/CPP/7zip/Bundles/Alone/_o"
  "../repo/bin"
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
  echo "警告: 未在预期路径发现 7-Zip 可执行文件(7zz/7za/7zr/7z)。" >&2
fi

# 兜底搜索
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find ../repo -maxdepth 6 -type f -perm -111 \( -name 7zz -o -name 7za -o -name 7zr -o -name 7z \) 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# 同步到 artefacts 并生成带模式后缀
mkdir -p ../artefacts/7zz
cp -f .bin/* ../artefacts/7zz/ 2>/dev/null || true

PRIMARY=""
for cand in 7zz 7za 7zr 7z; do
  if [ -f "../artefacts/7zz/$cand" ]; then
    PRIMARY="$cand"; break
  fi
done

if [ -n "$PRIMARY" ]; then
  cp -f "../artefacts/7zz/$PRIMARY" "../artefacts/7zz/${PRIMARY}.${mode}"
  chmod +x "../artefacts/7zz/${PRIMARY}.${mode}" || true
fi

popd
