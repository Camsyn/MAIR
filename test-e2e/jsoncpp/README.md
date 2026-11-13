# jsoncpp e2e

从仓库根目录运行：

```sh
RUNS=1000 METRICS_REPEAT=5 JOBS=4 bash test-e2e/jsoncpp/run.sh
```

首次运行下载固定提交 `8190e061bc2d95da37479a638aa2c9e483e58ec6`，使用
`build/xclang`、`build/xclang++` 编译 ASan/TSan × baseline/MAIR 四种变体。
需要已构建的 MAIR 编译器，以及含 libc++、libFuzzer 和 sanitizer runtime 的 LLVM 23。
`LLVM_BIN` 默认 `/home/camsyn/kits/llvm/23/bin`，可显式覆盖。

每种变体先运行检查合法 JSON、嵌套值和非法 JSON 的 oracle，再使用上游
`src/test_lib_json/fuzz.cpp` 运行固定随机种子的 libFuzzer。语料选择与
`evaluation/jsoncpp/fetch.sh` 一致，每次运行复制独立语料，避免前一次 fuzz
改写后续工作负载。库与 driver 均启用 `fuzzer-no-link` 覆盖插桩。

`build/metrics/` 保存体积、插桩点数、平均耗时及 perf 指标，
`build/mair-stats/` 保存 MAIR 编译统计。硬件 perf 事件不可用时标为
`unsupported`；sanitizer 或工作负载失败会终止运行。短预算运行用于功能验证，
不能据此推断稳定的性能收益。

后续运行默认复用二进制。编译器、MAIR 插件或构建参数变化后，使用：

```sh
JSONCPP_E2E_REBUILD=1 RUNS=1000 bash test-e2e/jsoncpp/run.sh
```

该开关把旧变体构建移到 `.temp/recyclebin/`，从头编译，保留旧产物。
可通过 `JSONCPP_E2E_SOURCE`、`JSONCPP_E2E_CC`、`JSONCPP_E2E_CXX` 指定源码缓存
和编译器，通过 `JSONCPP_E2E_MAX_LEN` 调整输入长度上限（默认 65536）。
