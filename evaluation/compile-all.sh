#! /bin/bash

# -----------------------------------------------------------------------------
# 脚本说明：
#   本脚本 compile-all.sh 用于批量编译指定 package 目录下的基准程序，支持多种 Sanitizer 组合模式。
#   可自动遍历所有支持的 mode，依次调用 package 目录下的 compile.sh 进行编译。
#   支持强制重新编译（-f），以及自定义 package 路径和编译模式。
#   2024-加：支持 -t 记录编译时间追踪（-ftime-trace），详见后述。
#
# 用法（Usage）:
#   ./compile-all.sh [package-path] [-f] [-t] [mode1 mode2 ...]
#
# 以下参数位置不限，可以混合使用
#   [package-path]    ：（可选）指定包含 compile.sh 的 package 目录，默认为当前目录
#   -f                ：（可选）强制重新编译（即使 artefacts 已存在也会重新编译）
#   -t                ：（可选）追踪编译耗时（导出 -ftime-trace）
#   [mode1 mode2 ...] ：（可选）仅编译指定的 mode（如 asan msan raw），不指定则编译全部支持的 mode
#
# 示例：
#   ../compile-all.sh
#   ./compile-all.sh ./binutils
#   ../compile-all.sh -f
#   ./compile-all.sh ./binutils -f
#   ./compile-all.sh ./binutils asan msan
#   ../compile-all.sh -f asan msan
#   ./compile-all.sh ./binutils asan -f
#   ./compile-all.sh ./binutils -t
#   ../compile-all.sh -f -t asan msan
#
# 典型用途：
#   用于自动化批量编译所有或部分 Sanitizer 组合下的可执行文件，便于后续评测和测试。
# -----------------------------------------------------------------------------


# 可用的mode列表
modes=(
    raw
    asan
    ubsan
    tsan
    msan
    # dbg
    # xsan-asan
    # xsan-asan-tsan
    # xsan-asan-msan
    # xsan-asan-ubsan
    # xsan-asan-tsan-ubsan
    xsan-asan-msan-tsan
    # xsan-asan-msan-ubsan
    # xsan-asan-msan-tsan-ubsan
)

enabled_modes=( )

force=0
workdir=""
track_time=0



# 处理参数，支持 -f, -t, <package-path>, 可以任意顺序混用
while [[ $# -gt 0 ]]; do
    case "$1" in
        -f)
            force=1
            shift
            ;;
        -t)
            track_time=1
            shift
            ;;
        *)
            # 如果 workdir 为空，且 $1 是一个目录，则将 $1 设置为 workdir
            if [ -z "$workdir" ] && [ -e "$1" ]; then
                workdir="$1"
            else
                enabled_modes+=("$1")
            fi
            shift
            ;;
    esac
done

# 如果 enabled_modes 为空，则将其设置为 modes
if [ ${#enabled_modes[@]} -eq 0 ]; then
    enabled_modes=("${modes[@]}")
fi

if [ -z "$workdir" ]; then
    workdir="$(pwd)"
fi

if [ ! -f "$workdir/compile.sh" ]; then
    echo "错误：未找到 $workdir/compile.sh"
    echo "请在包含 compile.sh 的目录下执行本脚本，或将该目录作为参数传递给本脚本"
    exit 1
fi

pushd $workdir

# 检查 repo 目录是否存在且非空，否则调用 package/fetch.sh
if [ ! -d repo ] || [ -z "$(ls -A repo 2>/dev/null)" ]; then
    if [ -f "$workdir/fetch.sh" ]; then
        echo "未检测到 repo 目录或目录为空，正在调用 $workdir/fetch.sh 拉取源码..."
        bash "$workdir/fetch.sh"
    else
        echo "错误：未找到 $workdir/fetch.sh，无法拉取源码"
        exit 1
    fi
fi


# 将 package 裁剪为目录名
package=$(basename "$workdir")

# 如果 track_time=1，导出相关环境变量
if [ $track_time -eq 1 ]; then
    export CFLAGS="-ftime-trace ${CFLAGS}"
    export CXXFLAGS="-ftime-trace ${CXXFLAGS}"
fi

for mode in "${enabled_modes[@]}"; do
    echo "=============================="
    echo "编译模式: $mode"
    skip=1
    if [ $force -eq 1 ]; then
        skip=0
    else
        # 检查 artefacts/.compiled/<package>/<mode> 是否已存在
        marker=".compiled/$mode"
        if [ -f "$marker" ]; then
            skip=1
        else
            skip=0
        fi
    fi

    if [ $skip -eq 1 ]; then
        echo "所有 $mode 产物已存在，跳过编译"
        continue
    fi

    export mode="$mode"
    start_time=$(date +%s)
    ./compile.sh
    if [ $? -ne 0 ]; then
        echo "编译失败，跳过 $mode"
        continue
    fi
    end_time=$(date +%s)
    compile_time=$((end_time - start_time))

    # 检查 temp/.bin 是否存在
    if [ ! -d temp/.bin ]; then
        echo "警告: temp/.bin 目录不存在，跳过 $mode"
        continue
    fi
    for bin in temp/.bin/*; do
        [ -f "$bin" ] || continue
        prog=$(basename "$bin")
        mkdir -p artefacts/"$prog"
        mv "$bin" "artefacts/$prog/$prog.$mode"
        # 标记产物已编译
        echo "已移动 $bin 到 artefacts/$prog/$prog.$mode"
    done
    mkdir -p ".compiled/"
    echo "Compile Time: $compile_time s" > ".compiled/$mode"

    # 追踪时间模式下收集 *.c.json 或 *.o.json
    if [ $track_time -eq 1 ]; then
        data_dir=".compiled/data-time-trace/$mode"
        rm -rf "$data_dir" && mkdir -p "$data_dir"
        # 查找所有 *.c.json
        found=0
        for f in $(find ./temp -type f -name "*.json" 2>/dev/null); do
            found=1
            # 使用 cp --parents 保留路径结构
            cp --parents "$f" "$data_dir" 2>/dev/null || \
                (mkdir -p "$(dirname "$data_dir/$f")" && cp "$f" "$data_dir/$f")
        done
        if [ $found -eq 1 ]; then
            # Move $data_dir/temp/* to $data_dir/
            mv "$data_dir/temp/*" "$data_dir/"
            rm -rf "$data_dir/temp"
            echo "已收集 $mode 编译追踪文件到 $data_dir/"
        else
            echo "未发现 $mode 编译追踪（.json）文件"
        fi
    fi
done

popd
