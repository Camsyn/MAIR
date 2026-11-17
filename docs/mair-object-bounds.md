# 对象边界消除（boe）

`ObjectBoundsEliminationPass`（CLI 名称 `boe`，`src/instrumentation/MAIR/Transforms/ObjectBoundsEliminationPass.cpp`）删除区域可证明落在某个“信任对象”之内的 Observation。sanitizer 保证信任对象 `[B, B + N)` 内的访问永远不会被报告，所以这些检查永远不会触发，删掉不丢报告。

## 开关与范围

- 只在 ASan 下、显式开启：`-mllvm -mair-asan-boe`（`ClMAIRBoundsElimination`，`src/instrumentation/llvm23/AddressSanitizer.cpp`），默认关闭。不开时 MAIR 检查的访问与现在完全相同，流水线里也没有 boe。
- 信任对象由 sanitizer 通过 `MAIROptimizationConfig::setTrustedObjectSize` 回调提供（`src/instrumentation/MAIR/MAIR.h`），MAIR core 不含任何 ASan 语义。没有回调时（所有 sanitizer 的默认），pass 直接返回，`-mair-stats` 也不打印。`MAOpt` 只在配置带回调时把 boe 加进默认流水线（`buildMAIRGraphDefaultPipeline` 的 `EliminateWithinTrustedObjects`）。
- TSan 不检查越界：TSan 插件里没有这个选项，传入即报 `Unknown command line argument '-mair-asan-boe'`；TSan 也不提供信任对象，在 `-mair-pipeline` 里写上 boe 也什么都不删（测试 `test/MAIR/TestCases/tsan/mair-asan-boe.c`）。
- `mair-opt -trust-constant-globals -passes=boe` 单独运行：用与 ASan 相同的 `ObjectSizeOffsetVisitor` 规则信任常量全局，但没有 ASan 的选项与初始化顺序条件。
- 调整流水线时，`-mair-pipeline` 只决定位置；仍要加 `-mllvm -mair-asan-boe`，否则没有信任对象。

## ASan 信任哪些对象

`instrumentFunctionViaMAIR` 里的回调，与 native `instrumentMop` 省略全局访问的条件一致，再限制为常量：

- `GlobalVariable` 且 `isConstant()`；
- `ClOpt && ClOptGlobals`，且 `!ClInitializers || GlobalIsLinkerInitialized(G)`；
- 大小取 ASan 自己的 `ObjSizeVis`（`ObjectSizeOffsetVisitor`，精确模式）对 `G` 本身的结果，偏移必须为 0。外部声明、可被替换（weak、linkonce、common）、extern_weak 或无大小的全局得不到大小，不信任。

ASan 只在访问的字节被 poison 时报告。常量全局 `[G, G + N)` 内的字节不会被 poison：模块 pass 只 poison 尾部 redzone；初始化顺序检查只 poison 有动态初始化（`IsDynInit`）的全局，IR 中 `isConstant()` 的全局没有动态初始化；用户 poison 与 libc++ 容器注解作用在可写内存上；全局不会被释放。所以界内访问永远不会报告。这里假设用户不会对只读数据调用 `__asan_poison_memory_region`。

ASan 先插桩函数、后插桩模块（`AddressSanitizerPass::run`），函数插桩时全局还是原类型，`ObjSizeVis` 得到的是不含 redzone 的大小。

### 不在范围内（TODO）

- 可变全局：用户可以 poison（`.temp/null/20261117_1506/D-object/exp/gpoison.c`：native 与 MAIR 都报 use-after-poison，删掉检查就丢了唯一的报告），libc++ 短字符串注解同理。
- 堆对象：需要排除 container-overflow、并发下的 use-after-free（需 `!PointerMayBeCapturedBefore`），并且 `malloc(n << 3)` 这类符号大小只在模 2^64 下等于区间长度，证明不成立。
- 见 D 组分诊报告（`.temp/null/20261117_1506/D-object/report.md`）第 1、2 点。

## 证明

偏移是区域地址的 SCEV 减去基址（`AbstractAccess::getAddressSCEV` 与 `getMinusSCEV`，SCEV 的指针基址不同就不证）。区域的大小、起点、stride、trip count 是图里的 `MAIRScalarExpr`，逐节点转回 SCEV（`getSCEV`）。整数统一扩展到至少索引宽度（偏移和 stride 符号扩展，大小和 trip count 零扩展）。对象不超过 2^32 字节、索引 64 位时，下面每条检查接受的值绝对值都小于 2^63，SCEV 的模运算与数学运算一致。

- 连续区间 `[o, o + s)`：`0 <= o <= N`，`s <= N`（无符号），`o + s <= N`。前两条保证 `o + s` 不回绕。单个访问是 `s` 为常量的情形。
- periodic 区域（起点 p、stride d、单次大小 b、trip count n）：`n <= 2^31`、`|d| <= 2^31`、`b <= N`，第一轮 `[p, p + b)` 和最后一轮 `[p + (n - 1) d, p + (n - 1) d + b)` 都在 `[0, N]` 内。`(n - 1) d` 不回绕，各轮起点都在首末两轮之间，所以整个区域都在界内。`n = 0` 时不访问，检查只会更保守。

每条界限依次尝试：

1. SCEV 自身的 `getSignedRange`/`getUnsignedRange`；
2. 程序点所在循环的 `applyLoopGuards` 之后的值域；
3. `isKnownPredicateAt`：支配程序点的条件；
4. LVI：把 SCEV 树按 `ConstantRange` 的运算组合起来，叶子（`SCEVUnknown`）取 `LazyValueInfo::getConstantRange(V, 程序点, UndefAllowed=false)`，每个节点再与 SCEV 自己的值域求交。上下界可以分别由不同的步骤证明。`UndefAllowed=false`：undef 不能当作 LVI 任选的值，程序运行时是什么值就是什么值。

程序点取 anchor 指令，edge site 取 From 块的 terminator。证明的是所有到达该程序点的执行，比 guard 条件更强。删除依赖 MAIR 的不变量：Observation 在其程序点的检查代表它的所有来源访问（`SPEC.md` 第 3 节）；被 ROE 删除的访问也被保留的 Observation 覆盖，覆盖者在界内，被覆盖者也在界内。证明以 LLVM IR 语义为准：`nsw`/`nuw`/`inbounds` 违反时结果是 poison，程序已有未定义行为，与 LPOC 依赖 SCEV 的 no-wrap 标志相同。

`-mair-stats` 中 `boe` 一行：`eliminated` 是删除数，`scev` 是只用前三步就证明的个数，`lvi` 是用到第 4 步的个数。

### 各方法的贡献与编译时间

43 个 e2e TU（D 组方法：`-O2`、libc++，`.temp/null/20261117_1720/WP6/e2e/m.sh`），原型同时实现了四种方法（`.tasks/WP6_20261117_1720/prototype-all-methods.cpp.txt`），每种方法在前面方法的事实上叠加：

| 方法 | 新增证明 |
| --- | --- |
| SCEV（值域、loop guards、`isKnownPredicateAt`） | 781 |
| LVI | +48 |
| ValueTracking（`computeConstantRange` 有符号/无符号、`computeKnownBits`，带 AC、程序点、DT） | +0 |
| SCCP 求解器（`SCCPSolver`，无 PredicateInfo） | +0 |

只在 SCEV 之后叠加 ValueTracking 仍是 +0；只叠加 SCCP，sqlite3 上 +1，这一个 LVI 也能证明。带 PredicateInfo 的 SCCP 会往 IR 里插 copy，不适合在插桩 pass 里用，没有尝试。

sqlite3 编译时间（机器负载约 17，3 次取最小）：不开 9.09 s，只用 SCEV 9.06 s，SCEV + LVI 9.19 s，四种全开 11.29 s（+24%，SCCP 在每次图访问时对整个函数求解）。所以只保留 SCEV 和 LVI；LVI 在第一次需要时才建（`LazyLVI`）。

## 流水线位置

`default<O1>` 里放在 `roe#2` 之后、`simplifygraph#2` 之前（`default<Og>` 里在 `roe#1` 之后）：

```
... → [locationmerge#1, liom#1, lpoc] → roe#2 → boe → simplifygraph#2
```

boe 只删除 Observation，不会产生新的覆盖。如果在 `roe#2` 之前删掉一个能覆盖别的访问的 Observation Y，`roe#2` 就不能再用 Y 删掉被它覆盖的 X，而 X 未必能单独证明在界内；放在之后，`roe#2` 先用 Y 删 X，boe 再删 Y，结果不会更差。最后的 `simplifygraph#2` 清理被删空的程序点。sqlite3 上三个位置（`simplifygraph#1` 之后、`roe#2` 之前、`roe#2` 之后）检查数相同（32192）。

## 结果

43 个 TU 的 ASan 静态检查数（`__asan_report_*`、`__asan_{load,store}*`、periodic 入口的调用点）：59069 → 58237（−832，−1.41%）。有变化的 TU：

| TU | 不开 | 开 | 删除（scev / lvi） |
| --- | --- | --- | --- |
| sqlite3 | 32969 | 32192 | 777（733 / 44） |
| 7z_Aes | 167 | 138 | 29（29 / 0） |
| 7zc_DeflateEncoder | 922 | 913 | 9（9 / 0） |
| re2_dfa | 1362 | 1354 | 5（5 / 0） |
| jsoncpp_json_writer | 908 | 904 | 4（3 / 1） |
| 7z_Sha256、7z_XzDec、7z_XzEnc、re2_parse、re2_regexp | | 各少 1 | 各 1 |

共删除 829 个 Observation（scev 781、lvi 48）；re2_dfa 删除 5 个 Observation 少了 8 个检查点，一个 Observation 可以对应多个检查调用点。

sqlite3 主要是 `sqlite3UpperToLower[c]`、`sqlite3CtypeMap[c]` 这类热路径查表（`c` 是 `unsigned char`）。

sqlite3 e2e workload（`test-e2e/sqlite3` 的 oracle 与 fuzzer，`-O2 -g -fsanitize=fuzzer-no-link`，在本 WP 的临时目录手工构建，`.temp/null/20261117_1720/WP6/rt.sh`）：构建时删除 765 个 Observation（scev 727、lvi 38）；两个变体的 oracle 都通过。fuzz 本身不可比：检查分支变了，覆盖随之不同，生成的输入也不同。改用固定输入集（751 个输入各执行 30 次，两变体交替各 6 次，`.temp/null/20261117_1720/WP6/fixed.sh`）：机器负载约 10 时 wall 与 user+sys 的差异方向相反，在噪声内；负载降到 2–7 后再跑 10 轮，user+sys 平均 7.397 → 7.364 s（−0.4%），最小 7.312 → 7.271 s（−0.6%）。没有硬件计数器可用，约 0.5% 的下降接近噪声。明细见 `.tasks/WP6_20261117_1720/worklog.md`。

## 测试

- `test/MAIR/TestCases/object-bounds-elimination.ll`：mair-opt 单测 boe；字节下标、`c + 1`、可变/外部/weak 全局、掩码下标、支配条件、循环内查表、LPOC 得到的区间与 periodic（界内删除，可能越界保留），以及不信任时什么都不删。
- `test/MAIR/TestCases/object-bounds-elimination-stats.ll`：`-mair-stats` 的 `scev`、`lvi` 计数。
- `test/MAIR/TestCases/asan/mair-safe-const-table.c`：开选项时 `T[(unsigned char)s[i]]` 的检查被删，`T[s[i] + 1]`、可变全局保留；不开时与现状一致；运行时越界的 `T[idx]` 报 global-buffer-overflow；boe 位于 `roe#2` 与 `simplify#2` 之间。
- `test/MAIR/TestCases/tsan/mair-asan-boe.c`：TSan 不接受该选项，流水线里写 boe 也不删。
- `src/instrumentation/MAIR/unittests/ObjectBoundsEliminationPassTest.cpp`：上述规则的单元测试，含负下标、跨越末尾的 2 字节访问、另一条路径上的条件、volatile 的可删除性。
