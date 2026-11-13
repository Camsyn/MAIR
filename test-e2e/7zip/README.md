# 7zip e2e

从仓库根目录运行：

```sh
METRICS_REPEAT=5 JOBS=4 bash test-e2e/7zip/run.sh
```

首次运行从 `mcmilk/7-Zip` 获取固定提交
`bd76c3ce3f8a4804274080e840d6a062454a7c73`（源码版本 25.01）。
这与 `evaluation/7zip` 使用同一仓库，但固定了版本。
复用上游 `CPP/7zip/Bundles/Alone2/makefile.gcc` 构建完整的 `7zz` CLI，
不修改上游源码。运行矩阵为 ASan/TSan × baseline/MAIR。

四种变体均使用上游的 `-O2 -g`，C++ 使用 libc++，禁用 ccache；
通过 Makefile 的 `CFLAGS_BASE2`、`CXXFLAGS_BASE2` 和 `LDFLAGS` 注入对应参数。
不启用汇编实现，以便压缩路径中的 C/C++ 内存访问参与插桩。
MAIR 编译统计保存在 `build/{asan,tsan}/mair/build.log`。

`workload.py` 生成约 4 MiB 的确定性文本和二进制，以及空文件和嵌套空目录，
包括含空格的文件名。每次在独立目录中使用 LZMA2、`-mx=1 -mmt=2` 压缩成
7z 归档，执行归档测试、解压，再逐字节比较完整文件树，并拒绝意外的符号链接。
这验证真实 CLI 的压缩、解压和 sanitizer 兼容性；不是 libFuzzer 测试。
编译、命令失败或内容差异都会立即终止。

需要已构建的 `build/xclang`、`build/xclang++`，以及含 libc++ 和 sanitizer
runtime 的 LLVM 23；另需 GNU Make、Git、Python 3.9+ 和 nm。
公共 metrics 脚本使用 LLVM 的 size/objdump 工具；perf 是可选项。
`LLVM_BIN` 默认 `/home/camsyn/kits/llvm/23/bin`，可以显式覆盖。
调用者的 sanitizer options 和 symbolizer 设置在每个变体的子 shell 中保留。

结果保存到 `build/metrics/`，包含体积、插桩点、平均耗时和 perf。
计时涵盖 Python 启动、输入生成、压缩、归档测试、解压及内容比较，
短 workload 的耗时不适合直接推断稳定的性能收益。
硬件计数器不可用时显示 `unsupported`。
临时文件、输入、归档和解压结果保留在 `.temp/null/{时间}/7zip-*/`，
重复运行会增加磁盘占用。

默认复用已有二进制。编译器、MAIR 插件或构建参数变化后运行：

```sh
SEVENZIP_E2E_REBUILD=1 bash test-e2e/7zip/run.sh
```

重建将旧变体目录移动到 `.temp/recyclebin/`，从空目录编译，保留旧产物。
`SEVENZIP_E2E_SOURCE`、`SEVENZIP_E2E_CC`、`SEVENZIP_E2E_CXX` 可指定源码缓存和
编译器；请使用绝对路径。已有源码必须匹配固定提交且无已跟踪文件修改。
脚本不执行上游 `clean`。
