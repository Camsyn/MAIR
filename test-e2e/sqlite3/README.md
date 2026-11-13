# sqlite3 e2e

从仓库根目录运行：

```sh
RUNS=1000 METRICS_REPEAT=5 JOBS=4 bash test-e2e/sqlite3/run.sh
```

首次运行下载 SQLite 3.33.0 的官方 amalgamation 和 source zip，分别校验 SHA256，
并校对 source 的 `manifest.uuid` 和 amalgamation 的 `SQLITE_SOURCE_ID`。
固定提交为 `fca8dc8b578f215a969cd899336378966156154710873e68b3d9ac5881b0ff3f`。
这与 `evaluation/sqlite3/fetch.sh` 的早期快照 `c78cbf2e86850cc6` 不同。
使用发布版 amalgamation 可以直接编译 `sqlite3.c`，避开上游生成流程中的删除操作，
也不需要 Tcl 或 Autoconf。

每种变体只编译一次静态 SQLite 库，链接上游 `test/ossfuzz.c` 和本目录的功能
oracle。运行矩阵为 ASan/TSan × baseline/MAIR。oracle 检查内存数据库中的建表、
参数绑定、查询、事务及错误返回；纯 SQL 种子覆盖不同执行路径。
库和 driver 都使用 `-O2 -g -fsanitize=fuzzer-no-link`，保留 evaluation 的
`SQLITE_DEBUG` 及资源限制宏。四种变体共用这些设置。

需要已构建的 `build/xclang`、`build/xclang++`，以及含 libc++、libFuzzer 和
sanitizer runtime 的 LLVM 23；另需 CMake、Ninja、curl、unzip、sha256sum 和 nm。
`LLVM_BIN` 默认 `/home/camsyn/kits/llvm/23/bin`，可以显式覆盖。

每次 fuzz 都从跟踪的 SQL seeds 复制独立语料，使用 `-seed=1`，默认输入长度上限
65536。覆盖插桩不同可能导致后续 fuzz 输入不同；短预算用于 e2e 功能验证，
不足以得出稳定的性能收益结论。TSan 变体用于检查插桩与 runtime 的兼容性，
这里的单线程 workload 不承担并发竞争检出测试。

结果保存到 `test-e2e/sqlite3/build/metrics/`，包含体积、插桩点、平均耗时和 perf。
MAIR 编译统计保存在 `build/{asan,tsan}/mair/build.log`。硬件计数器不可用时显示
`unsupported`；编译、oracle 或计时 workload 失败会立即终止。
每个变体使用独立子 shell，保留调用者的 `ASAN_OPTIONS`、`TSAN_OPTIONS` 和
对应 symbolizer 设置。

默认复用已有二进制。编译器、MAIR 插件或构建参数变化后运行：

```sh
SQLITE3_E2E_REBUILD=1 RUNS=1000 bash test-e2e/sqlite3/run.sh
```

重建会把旧变体构建移动到 `.temp/recyclebin/` 后从头编译，保留旧产物。
可通过 `SQLITE3_E2E_SOURCE`、`SQLITE3_E2E_CC`、`SQLITE3_E2E_CXX` 指定源码缓存和
编译器，通过 `SQLITE3_E2E_MAX_LEN` 调整输入上限。源码缓存目录请使用绝对路径。
