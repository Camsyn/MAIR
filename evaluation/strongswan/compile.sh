#!/bin/bash
set -e  # 只要有一步失败，脚本立即退出

export PROJ=strongswan

# 使用与 tpm2-tss 相同的整体流程与变量约定
# 启用共享库与基础插件，便于运行时按需加载
CONFIG_OPTIONS="--enable-load-tester"
export SUBJECT=$PWD
export LDFLAGS="${LDFLAGS:+$LDFLAGS }-lpthread"
export ADDITIONAL="-DFORTIFY_SOURCE=2 -fstack-protector-all -fno-omit-frame-pointer -Wno-error"
export CXXFLAGS="$CXXFLAGS $ADDITIONAL"
export CFLAGS="$CFLAGS $ADDITIONAL"

# 模式：优先位置参数，其次环境变量，默认 raw
mode="${1:-${mode:-raw}}"

# 激活编译标志（C 项目，第二参为 false）
source ../activate_compile_flags.sh "$mode" false

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR="$ROOT_DIR/repo"
BUILD_DIR="$ROOT_DIR/temp"

rm -rf "$BUILD_DIR" && mkdir "$BUILD_DIR"

# 生成 configure
pushd "$REPO_DIR"
if [ ! -f "configure" ]; then
    if [ -x ./autogen.sh ]; then
        echo "生成configure脚本..."
        ./autogen.sh
    else
        echo "错误: 缺少 configure，且不存在 autogen.sh" >&2
        exit 1
    fi
fi
popd

pushd "$BUILD_DIR"
rm -rf *
rm -f config.cache
# 预先确定按模式隔离的输出目录，并将其作为编译期内置插件目录（PLUGINDIR）
ARTEFACTS_DIR="$ROOT_DIR/artefacts/charon"
PLUGINS_DIR_MODE="$ARTEFACTS_DIR/plugins/$mode"
LIB_DIR_MODE="$ARTEFACTS_DIR/lib/$mode"
mkdir -p "$PLUGINS_DIR_MODE" "$LIB_DIR_MODE"

ac_cv_func_mallinfo2=no "$REPO_DIR"/configure $CONFIG_OPTIONS \
  --with-plugindir="$PLUGINS_DIR_MODE" \
  --cache-file=/dev/null
make clean || true
make -j

# 在构建目录收集到 .bin
mkdir -p .bin

# strongSwan 常见工具，charon 除外（它需要占用各种诸如路由表、特定目录文件夹等。一个电脑只有一份的资源，很难并行测试）
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
      # 若为 charon，处理 libtool 包装器，优先选择真实 ELF
      if [ "$name" = "charon" ]; then
        if [ -f "$dir/.libs/charon" ]; then
          src="$dir/.libs/charon"
        elif [ -f "$dir/.libs/lt-charon" ]; then
          src="$dir/.libs/lt-charon"
        fi
      fi
      cp "$src" ".bin/$name"
      # 若仍非 ELF（可能仍是包装脚本），再尝试兜底从 .libs 取可执行
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

# 确保不保留任何 charon 相关的产物目录
rm -rf ../artefacts/charon 2>/dev/null || true

if [ "$found_any" -eq 0 ]; then
  echo "警告: 未在预期路径发现 strongSwan 可执行文件。" >&2
fi

# 兜底搜索：抓取 src 下所有可执行文件
if [ "$found_any" -eq 0 ]; then
  mapfile -t EXTRA_FOUND < <(find ./src ../repo/src -maxdepth 3 -type f -perm -111 2>/dev/null || true)
  for f in "${EXTRA_FOUND[@]:-}"; do
    [ -f "$f" ] || continue
    cp -f "$f" ".bin/$(basename "$f")"
    found_any=1
  done
fi

chmod +x .bin/* 2>/dev/null || true

# 显式排除 charon（即使兜底搜索复制到了 .bin）
rm -f .bin/charon 2>/dev/null || true

# 同步到 artefacts：为每个可执行生成 artefacts/<name>/<name>.<mode>
for f in .bin/*; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  # 跳过 charon 的打包
  if [ "$name" = "charon" ]; then
    continue
  fi
  out_dir="../artefacts/$name"
  mkdir -p "$out_dir"
  cp -f "$f" "$out_dir/${name}.${mode}"
  chmod +x "$out_dir/${name}.${mode}" || true
  # 为 charon 额外打包插件与共享库
  if [ "$name" = "charon" ]; then
    plug_out="$out_dir/plugins/$mode"
    lib_out="$out_dir/lib/$mode"
    mkdir -p "$plug_out" "$lib_out"
    # 收集插件 *.so（包含符号链接），并保持属性
    while IFS= read -r -d '' p; do
      cp -a "$p" "$plug_out/"
    done < <(find "$BUILD_DIR" -path "*/plugins/*" \( -type f -o -type l \) -name "*.so*" -print0 2>/dev/null || true)
    # 收集核心共享库（如 libstrongswan / libcharon，包含符号链接）
    while IFS= read -r -d '' s; do
      cp -a "$s" "$lib_out/"
    done < <(find "$BUILD_DIR" \( -type f -o -type l \) \( -name "libstrongswan.so*" -o -name "libcharon.so*" \) -print0 2>/dev/null || true)
    echo "[$mode] 已打包 $(ls -1 "$plug_out" 2>/dev/null | wc -l) 个插件到 $plug_out；共享库放在 $lib_out" >&2
  fi
done

popd
