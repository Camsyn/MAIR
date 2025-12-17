#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

export PROJ=tpm2-tss

rm -rf temp && mkdir temp

CONFIG_OPTIONS="--disable-shared --disable-doc --disable-scripts --enable-unit --disable-integration"
export SUBJECT=$PWD
export LDFLAGS=-lpthread
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# 模式：优先位置参数，其次环境变量，默认 raw
mode="${1:-${mode:-raw}}"

source ../activate_compile_flags.sh "$mode" false

# 首先生成configure脚本
pushd repo
if [ ! -f "configure" ]; then
    echo "生成configure脚本..."
    ./bootstrap
fi
popd

pushd temp
rm -rf *
../repo/configure $CONFIG_OPTIONS
make clean
make -j

# 将 tpm2-tss 下的指定可执行文件移动到 ../artefacts 目录
mkdir -p .bin

# tpm2-tss 主要生成库文件，但也有一些工具
# 查找可执行文件
find . -type f -executable -name "tss2*" -o -name "test*" | head -10

# 收集所有可执行文件
find . -type f -executable \( -name "tss2*" -o -name "test*" -o -name "*test*" \) -exec cp {} .bin/ \;

popd

# 收集可执行文件到 temp/.bin
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
  echo "警告: 未在预期路径发现 tpm2-tss 可执行文件。" >&2
fi

# 兜底搜索
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find ./test ./test/unit -maxdepth 2 -type f -perm -111 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# 同步到 artefacts 并生成带模式后缀
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
