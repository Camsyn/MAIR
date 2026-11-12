# MAIR 审查、重构与修复计划

- 基线：`29342887`（main），LLVM 23.0.0git（`/home/camsyn/kits/llvm/23`）。
- 基线测试：`ninja -C build check-mair` 通过，202 项单元测试，lit 27 通过、1 unsupported。
- 审查范围：`src/instrumentation/MAIR/` 全部源码，以及 `llvm23/AddressSanitizer.cpp`、`llvm23/ThreadSanitizer.cpp` 中的 MAIR 接入点。runtime 与上游 sanitizer 主体未逐行审查。
- 任务资料：`.archive/mair_review_refactor_20261111_2102/`（复现用例在 `probes/`，差分脚本 `difftest.sh`，过程记录 `worklog.md`）。该目录被 `.gitignore` 忽略，只在本机存在；F0–F3 的复现 IR 已同时写进对应的单元测试。
- 分支：`claude/mair-review-refactor`，每个条目一个 conventional commit。

本计划同时是实施依据和事后 review 的对照表。每个条目写明：现象、根因、改法、验证方式、不做什么。实施结果回填在文末“实施记录”。

## 1. 严重 bug（均已用最小 IR 复现）

复现命令均为 `build/mair-opt <选项> .archive/mair_review_refactor_20261111_2102/probes/<文件>`。

### F0 优化结果随进程地址变化（输出不确定）

- 复现：对同一输入反复运行 `mair-opt -O1`（例如 jsoncpp 的任一翻译单元），输出的 observation 顺序在不同运行之间不同；关闭 ASLR（`setarch -R`）后稳定。`-Og`、`roe`、`locationmerge` 都受影响。
- 影响：程序点内 observation 的顺序决定物化后插桩调用的顺序，因此同一份源码两次编译会得到不同的目标码，构建不可复现；`.ma` golden 也只是恰好稳定。
- 根因：ROE 的 `compareForCoverage` 和 LocationMerge 的 `compareForLocationMerge`、`compareAbstractValue` 用 `std::less` 比较 `Value*`、`MemoryObject*` 的地址来排序。
- 修复：`MemoryObject` 记录图内创建序号 `getID()`，两个比较器按序号排序；符号偏移之间没有与运行无关的顺序，改为视作等价，由稳定排序保持原有相对顺序（同时只在常量偏移相等时才比较 size，保证比较器仍是严格弱序）。
- 等价性：与基线相比，优化后保留的 observation 集合不变，只有程序点内的顺序、以及个别新建程序点的编号不同（351 组输入×pipeline 归一化比较全部一致）。
- 这一条排在最前面实施：后面的等价重构要靠逐字节差分验证，前提是输出确定。

### F1 LIOM 把并非每次进入循环都执行的 observation 提升到 preheader（误报）

> 下面是第一轮的修法。按 review 意见返工后的最终做法见第 10 节。

- 复现：`-passes=liom liom_outer_backedge.ll`。内层循环里的 `store %p` 只在 `%j > 5` 时执行，却被提升到内层 preheader 且使用 `AlwaysGuard`，于是 `%j <= 5` 的每轮外层迭代都会观测一个程序从未访问的地址。
- 影响：所有配置。ASan 表现为误报，TSan 表现为虚假的竞争访问。
- 根因：`MAIRLoopInfo::canHoistToPreheader`（`Analysis/LoopAnalysis.cpp`）用 `PDT->dominates(Source, Preheader)` 证明“进入循环必执行”。后支配只保证“最终会执行”，这条路径可以先离开内层循环、绕外层回边再回来。
- 修复：增加必要条件“Source 所在块支配该循环的每个 exiting block”（`Loop::getExitingBlocks` + `DominatorTree::dominates`）。这与 LICM 的 `allLoopPathsLeadToBlock` 是同一判据；不直接用 `LoopSafetyInfo::isGuaranteedToExecute`，因为它会把任何可能抛异常的调用都当作阻断，接受域明显变窄，而这类阻断在 MAIR 里由 barrier trait 负责。
- 不损失合法提升：若 Source 不支配某个 exiting block，就存在一条进入循环后不经过 Source 直接离开的路径，原提升本来就不成立。

### F2 PostDom 冗余消除跨越循环回边（漏报）

> 下面是第一轮的修法。按 review 意见返工后的最终做法见第 10 节。

- 复现：`-passes=roe pdom_backedge.ll`。块 A 在 `%i < 3` 时写 `base[i]`，其 observation 被块 B 的同名 `%p` 写“覆盖”并删除；但 B 只有绕回边进入下一轮迭代才会执行，那时 `%p` 已是另一个地址。`base[0..2]` 的写不再被观测。
- 影响：允许 PostDom 覆盖的配置，即 generic 与 TSan。ASan 配置为 `DomCoverageOnly`，不受影响。
- 根因：循环作用域图里的 PostDom 边按整个函数的后支配树建立（`MAIRGraph::build`，`MAIR.cpp`）。“B 后支配 A”可以只通过回边成立，而循环内定义的 SSA 值跨迭代不是同一个值。现有的 `sameLoopForPostDom` 只挡住了“循环内与循环外”这一种情形。Dom 边不存在这个问题（定义点支配两个使用点时，最后一次定义之后必然先经过支配者）。
- 修复：在建图时只为“迭代内”的后支配关系建 PostDom 边。对循环 L 内的块 T 及其（同作用域的）后支配者 F，若存在一条从 T 出发、在到达 F 之前就回到 L 的 header 或离开 L 的路径，则不建这条边；F 就是 header 时同样不建。检查是限制在 L 内的一次 DFS，量级与已有的 `hasBwdBarrierPath` 相同。
- 取舍：这是保守做法，循环不变地址“绕回边被覆盖”的机会会丢失；这类 observation 本来由 Dom 覆盖和 LIOM 处理。选择在建图处修而不是在 ROE 里再加一个使用点判断，是为了让“循环作用域图描述一次迭代”成为图本身的不变量。
- 连带影响：含循环的 lit golden 中 `preds/succs` 列表会变化，用 `utils/update_mair_test_checks.py` 重新生成并逐项核对。

### F3 不可约环的块识别不完整（漏报）

> 下面是第一轮的修法。按 review 意见返工后的最终做法见第 10 节。

- 复现：`-O1 irreducible_variant.ll`。不可约环 `{A,B,C,D}` 中块 C 没有被标记为 unsupported，其 observation 被跨“迭代”的后支配者删除。
- 根因：`collectUnsupportedLoopBlocks`（`MAIR.cpp`）用手写 DFS，只标记发现非自然回边那一刻 DFS 栈上的块；环上经由其他路径访问的块被漏掉。
- 修复：改用 LLVM 的 `CycleInfo`。遍历环层次，把每个 `!isReducible()` 的环的全部块标记为 unsupported。可约环与 `LoopInfo` 的自然循环一一对应，不受影响。这同时删除约 45 行手写 DFS。
- 取舍：嵌在不可约环内部的自然循环也会被整体钉住，比现在保守，与现有 TODO “非标准循环暂不优化”一致。

## 2. 性能重构（有测量依据，要求输出逐字节不变）

测量输入是合成 IR（`.temp/null/20261111_2102/ir/`），基线二进制为 Debug 构建，数字只用于判断量级。

| 输入 | pipeline | 基线耗时 | 规模减半时 | 热点 |
| --- | --- | --- | --- | --- |
| 单块 4000 个 load | `simplifygraph` | 4.2 s | 1.0 s | `contractProgramPoint` → `prependObservationsFrom` 占 87% |
| 1500 个菱形 | `simplifygraph,roe` | 36.2 s | 10.3 s | `RedundantObservationAnalysis::run` 占 97% |

jsoncpp 三个翻译单元的 `-O1` 全程只要 0.1–0.2 s，说明常规代码不是问题，问题出在大函数上的平方行为。

### R1 Observation 改为堆分配，收缩时移动而不是复制

- 现状：每个 `ProgramPoint` 自带一个 `SpecificBumpPtrAllocator<Observation>`。收缩一条长度为 n 的链时，累积的 observation 每一步都被重新构造并重新注册，共 O(n²) 次；被删除的 observation 直到图析构才释放；每个带 observation 的程序点还独占一个 4 KB 的 slab。
- 改法：observation 用 `new`/`delete` 管理，由所在程序点通过 `simple_ilist::eraseAndDispose`/`clearAndDispose` 释放；`contractProgramPoint` 用 `simple_ilist::splice` 整体移动节点，只更新 parent 指针，并在两端“是否位于 unsupported 块”不同时刷新优化属性。
- 等价性：移动后的 observation 内容、顺序、所属 MemoryObject 与复制版本相同。两点差异需要 review 确认可接受：observation 对象身份在收缩后保持不变（原来是新对象）；MemoryObject use-list 中的相对顺序不再因复制而改变（生产代码只有 verifier 做成员检查，不依赖顺序）。
- 不做：LIOM、COM、LPOC 的“复制后删除”暂不改用移动，没有测到它们是热点。

### R2 冗余分析改为按强连通分量计算的程序点可达性

- 现状：分析按 `(目标程序点, 来源 observation)` 逐条事实传播，每条事实一次集合插入，空间与时间都是 O(程序点数 × observation 数)。
- 改法：`canPropagate` 只依赖边，不依赖来源，所以可达性是程序点级的。把“启用的边”反向后用 LLVM 的 `scc_iterator` 求强连通分量；反向图的分量按“前驱先出”的顺序给出，因此一遍扫描即可得到每个分量的“可到达它的来源程序点集合”（`SparseBitVector`），不需要迭代到不动点。同一分量内的程序点共享一个集合；程序点只有在环上才能到达自己。查询时沿目标的 `MemoryObject` use-list 找同 base 的候选来源，再查集合并调用 `Observation::covers`。
- 等价性：`covers` 本来就要求同一 base，`ExcludedSources`、同程序点排除、延迟删除的顺序都不变，因此每次查询的布尔结果不变。
- 接口变化：`AvailableObservationSet` 和 `RedundantObservationInfo::getIn` 被 `RedundantObservationInfo::isCovered` 取代（两者都没有测试或其他调用方）。

## 3. 验证方式

- 每个条目提交前跑 `ninja -C build check-mair`，全绿才提交。
- R1、R2 是纯重构：用 F0 修复后保存的二进制 `.temp/null/20261111_2102/mair-opt.f0` 做差分（原始基线输出不确定，无法逐字节比较），对 `test/MAIR/TestCases/*.ll`、jsoncpp 三个翻译单元、合成输入和 `probes/` 在 raw、`-Og`、`-O1` 及各单 pass pipeline 下比较完整输出，要求逐字节相同。因此实施顺序为 F0、R1、R2、F3、F2、F1。
- F1–F3 测试先行：先加会失败的单元测试或 lit 用例，再改实现。修复后重新做差分，逐项解释每一处输出变化。
- ASan/TSan 的 `mair-*` 接入测试在全部条目完成后跑一遍。
- C++ 改动用 `git clang-format`（`src/instrumentation/.clang-format`）只格式化改动行。

## 4. 审查中发现但本轮不改的事项

- SCEV 耗时：600 个串行循环上 `lpoc` 的时间几乎全在 `ScalarEvolution` 内部（`opt -passes='print<scalar-evolution>'` 在同一输入上同样慢），不是 MAIR 的问题。
- `findValueForSCEV`（LPOC）在回退路径上对整个函数的每条指令调用 `getSCEV`，可能是平方行为，但没有测到，需要真实输入再判断。
- “值在某点可用”有三份实现：已在第 6 节 M5 合并。
- `addressSCEV` 与 `regionAddressSCEV`（LPOC）几乎相同，只在零偏移时不同：已在第 7 节 N1 合并，用参数区分两种行为。
- 仅被单元测试使用的接口：`MAIRGraph::isAncestor`、`MAIRLoopInfo::getLoopFor`、`MAIRLoopInfo::getTopInvariantLoop`。是否删除由维护者决定。
- `MAIRGraph::getLoopGraph` 每次调用都线性遍历整棵作用域树，而 LIOM/LPOC 的调用方查询的总是当前图自己的循环，可以直接取 `getLoopPreheader()`。
- min/max 表达式用 `icmp`+`select` 降级：已在第 7 节 N3 改用 `llvm.smin` 等 intrinsic。
- SimplifyGraph 每次收缩后从头扫描，理论上是平方的；在测试输入上不是热点（9000 个程序点约 0.15 s），且改成增量扫描需要证明候选顺序不变，暂不动。

## 5. 实施记录

最终状态：`ninja -C build check-mair` 通过，209 项单元测试（基线 202，新增 7），lit 27 通过、1 unsupported；ASan 6 项、TSan 5 项 `mair-*` 接入测试通过。ASan/TSan 的其余测试、MSan 和 XSan 插件没有运行。

| 条目 | commit | 验证 |
| --- | --- | --- |
| 计划 | `00441f86` | — |
| F0 | `369f0f97` | 新二进制 15 种输入×pipeline 各运行 8 次输出一致；与基线归一化比较 351 组一致。新增 `OrdersObservationsByMemoryObjectCreation` |
| R1 | `f0c449c8` | 与 F0 二进制逐字节比较 364 组一致。4000 个 load 的 `simplifygraph`：4.02 s / 1.5 GB → 0.23 s / 25 MB。新增 `ContractionMovesObservationsInSourceOrder` |
| R2 | `cf307f64` | 逐字节比较 369 组一致（含 1500 个菱形）。该输入的 `simplifygraph,roe`：34.9 s / 1.2 GB → 1.0 s / 61 MB |
| F3 | `a8697722` | 369 组中 12 组不同，全部是两个不可约环用例，差异为多保留 1 条 observation。新增 `EveryBlockOfAnIrreducibleCycleIsPinned` |
| F2 | `85381729` | 369 组中 108 组不同：102 组只有 `preds/succs` 列表变化，6 组是 `pdom_backedge.ll` 多保留 1 条 observation。jsoncpp 上保留的 observation 不变。新增两个测试（负例与同迭代内仍可消除的正例） |
| F1 | `fa635591` | 369 组中 3 组不同，全部是 `liom_outer_backedge.ll`，observation 留在原块。新增两个测试（负例与仍可提升的正例） |
| 接入测试 | `5ff029be` | 见偏差 3 |

以上耗时均为 Debug 构建下的单次测量。

与计划的偏差：

1. F0 不在最初的审查结论里，是做差分验证时发现基线与自身比较都不一致才查出来的，因此补进计划并排到最前。
2. R2 原计划用位向量加工作表迭代，实施时改为按强连通分量一遍算出，计划正文已同步改写。
3. F0 使程序点内的顺序跟随源码中首次访问的顺序，`test/tsan/TestCases/mair-basic.cpp` 原先期望先 `write` 后 `read`（地址顺序碰巧如此），改为先 `read` 后 `write`。这是产出目标码的可见变化。
4. F2 没有重新生成 lit golden：现有 lit 用例都没有检查循环内的 PostDom 边，27 项原样通过。

## 6. 可维护性重构（第二轮）

第一轮只做了 bug 修复和性能重构，可读性、去重、复用 LLVM API 这三类被列进第 4 节没有实施。本节补做，全部是不改行为的重构。

- 分支：`claude/mair-refactor-cleanup`，基于 `1482b67a`（第一轮分支 `claude/mair-review-refactor` 的末端），在独立 worktree `.claude/worktrees/mair-refactor` 中完成，没有改动主检出目录。
- 任务资料（差分脚本、一次性改写脚本、worklog）在该 worktree 的 `.archive/mair_maintainability_refactor_20261112_1452/`，被 `.gitignore` 忽略；删除 worktree 会一并删掉它。
- 规模：`src/instrumentation/MAIR` 生产代码 17 个文件，+357 / −556 行；测试 +52 行（1 个新单元测试）。
- 最终状态：`ninja -C build check-mair` 通过，210 项单元测试，lit 27 通过、1 unsupported；ASan 6 项、TSan 5 项 `mair-*` 接入测试通过。其余 sanitizer 测试没有运行。

| 条目 | commit | 做了什么 | 用到的 LLVM API |
| --- | --- | --- | --- |
| M1 | `6fa6a493` | 作用域遍历的 const/非 const 两份手写工作表合成一份；六处重复的“找某条边”lambda 收成 `findEdge`；非 const 的代表点 getter 转调 const 版本；移动构造与移动赋值共用 `moveFrom`；`getLoopGraph` 查询自身循环时不再遍历整棵树 | `llvm::depth_first`（复用已有的作用域树 `GraphTraits`） |
| M2 | `83da3cb6` | 文本 dump、DOT、verifier 各写一遍的站点种类名、边种类名、guard 来源列表，改为共用 `ProgramPointSite::getKindName`、`getEdgeKindName`、`AnyProvenanceExecutedGuard::print`；三个 `toString` 共用一个模板；值名直接打印到流，不再经过临时字符串 | `llvm::ListSeparator` |
| M3 | `280ef2c9` | 六个 pass 开头重复的 6 行统计样板收进 `MAIRStatScope` 构造函数；统计字段从运行时字符串匹配改为 `MAIRStatField` 枚举，拼错在编译期报错；删除 `mairStatsEnabled`、`mairStatFunctionName`。**这个提交引入了一处性能回归，见第 9 节** | LLVM 统计量在关闭统计时本身是空操作类，因此去掉了 `#if LLVM_ENABLE_STATS`（这个推理是错的：空操作类不会阻止参数求值） |
| M4 | `843caa97` | 三种写法的手写溢出判断换成 LLVM 接口；LPOC 的常量区间大小只算一次；字符串错误不再显式传 `inconvertibleErrorCode` 或经过本地包装 | `checkedAddUnsigned`、`checkedMulUnsigned`、`createStringError(const Twine &)` |
| M5 | `ffec1eba` | 循环分析、COM、物化三份“值在某点是否可用”合并为 `mair::isValueAvailableAt`（`Value` 与 `AbstractValue` 两个重载） | `DominatorTree::dominates(const Value *, const Instruction *)` |
| M6 | `0bd31eb3` | `Observation::fromInstruction` 对 load/store/atomicrmw/cmpxchg 的指针与大小不再逐类手写；`lowerExpr` 先按顺序降级操作数再生成指令，四种 min/max 共用一个 helper | `MemoryLocation::get` |

验证方式（每个条目提交前都跑）：

- `ninja -C build check-mair`。
- `mair-opt` 输出与重构前二进制逐字节比较：36 个输入 × 13 种选项（文本、`-emit-llvm`、`-mair-stats`、`print<stat>`、各 pass 组合）共 546 组，六个条目全部 0 处不同。脚本 `difftest.sh`。
- M2 另比较 `-emit-dot` 输出 30 组（屏蔽节点地址后），0 处不同。
- M5、M6 改到物化路径，另用 ASan/TSan 经 MAIR 路径编译接入测试、jsoncpp 和一个循环探针，共 38 次编译，插桩后的 IR 与重构前逐字节一致。

验证的局限：

- 上面 38 次编译没有一次触发标量表达式降级（生成的 IR 里没有 `mair.expr.*`），所以 M6 的 `lowerExpr` 改写只由物化单元测试覆盖，没有端到端差分。
- 新增的 `LowersExpressionOperandsInOrder` 在旧代码上也通过：旧代码的操作数顺序依赖调用参数求值顺序，clang 恰好从左到右。这个测试固定的是今后的行为，不能证明旧代码有问题。
- M5 的接受域是靠读代码确认一致的（三处都只接受常量、参数和支配插入点的指令），另有 546 组差分佐证，没有专门的新测试。

这一轮没有做、随后在第 7 节按要求补做的四项：合并两份地址 SCEV 构造、min/max 改用 intrinsic、执行次数改用 `getTripCountFromExitCount`、拆分 `MAIR.cpp`。

## 7. 后续四项（第三轮）

第二轮把这四项列为“不做”，理由是会改变行为或属于结构取舍。维护者明确要求做，并指定地址 SCEV 的合并用参数控制零偏移时的行为。

- 分支仍是 `claude/mair-refactor-cleanup`，接在第二轮之后。
- 最终状态：`ninja -C build check-mair` 通过，210 项单元测试，lit 27 通过、1 unsupported；ASan 6 项、TSan 5 项 `mair-*` 接入测试通过。其余 sanitizer 测试没有运行。

| 条目 | commit | 做了什么 | 是否改变产出 |
| --- | --- | --- | --- |
| N1 | `71fc632a` | 两份地址 SCEV 构造合并为 `AbstractAccess::getAddressSCEV(SE, ZeroOffsetBase)`。`Opaque` 把零偏移的 base 当作不透明的 `SCEVUnknown`（区间合并用），`Analyzed` 用 ScalarEvolution 自己的表达式（LPOC 找递推用） | 否：546 组逐字节一致 |
| N2 | `f9212360` | `getExactExecutionCountSCEV` 中“站点在出口判断之前”的一支改用 `ScalarEvolution::getTripCountFromExitCount`；“之后”的一支仍是 backedge 次数零扩展 | 理论上可能，实测没有：546 组逐字节一致，其中包含带 `+1` 的符号执行次数 |
| N3 | `c75e7e51` | min/max 表达式的降级从 `icmp`+`select` 改为 `llvm.smin`/`umin`/`smax`/`umax` | 是：这类表达式物化出的 IR 从两条指令变为一次 intrinsic 调用，值相同 |
| N4 | `c55265aa` | `MAIR.cpp`（2073 行）按职责拆成 `MAIRGraphBuilder.cpp`（241 行，建图）、`MAIRRegion.cpp`（535 行，标量表达式与各类区域）、`MAIRObservation.cpp`（429 行，observation 与优化配置）和 `MAIR.cpp`（863 行，程序点与图容器），共用的少量定义放在 `MAIRInternal.h`（76 行） | 否：546 组逐字节一致 |

验证与局限：

- N1、N2、N4 用与第二轮相同的 546 组 `mair-opt` 差分，参考二进制是第三轮开始前的构建。
- N2 的等价性只在差分输入上成立。LLVM 在能证明加一不溢出时会先加再扩位，这样的循环会得到形状不同（值相同）的表达式；差分输入里没有出现，所以没有观察到，也没有新增针对它的测试。
- N3 没有任何端到端覆盖：546 组差分和 32 次 ASan/TSan 编译都不产生 min/max 表达式，只有物化单元测试 `LowersCompletePeriodicExpressionDag` 覆盖，其断言已从统计 `icmp` 改为统计四种 intrinsic 各出现一次。
- N4 用脚本切分并断言各片段能原样拼回原文件；另用 `check_split.py` 比较拆分前后的代码行多重集，差异只有 include、命名空间括号、新头文件里的声明，以及 6 个共享 helper 的链接属性（`static` 改为 `inline` 或外部链接）。
- N4 连带把 `docs/mair-loops.md` 里指向 `postDominatesWithinIteration` 的路径改为 `MAIRGraphBuilder.cpp`。`docs/dev-MAIR/2-ConstructMAIRGraph.md` 里还有两处提到 `MAIR.cpp`，那是标明“不是当前规范”的历史设计记录，没有改。

仍然没有做的：

- 仅被测试使用的接口（`isAncestor`、`MAIRLoopInfo::getLoopFor`、`getTopInvariantLoop`）的删除：属于接口取舍。
- masked load/store 的操作数下标仍是手写的，没有核对 LLVM 是否有现成访问器。
- `LoopPeriodicObservationCombinePass.cpp`（约 600 行）没有拆分。

任务资料在 worktree 的 `.archive/mair_followup_refactor_20261112_1557/`，被 `.gitignore` 忽略。

## 8. 去重（第四轮）

第三轮之后又做了一遍只读审查，列出 6 项不改行为的重构和一组需要取舍的接口问题。维护者要求先做前 5 项。

- 分支仍是 `claude/mair-refactor-cleanup`，接在第三轮之后。
- 最终状态：`ninja -C build check-mair` 通过，210 项单元测试，lit 27 通过、1 unsupported；ASan 6 项、TSan 5 项 `mair-*` 接入测试通过。其余 sanitizer 测试没有运行。

| 条目 | commit | 做了什么 |
| --- | --- | --- |
| D4 | `c915af6d` | `MAIRGraph::registerObservation` 不再重复“按配置初始化，再在不可约环块里全部置否”，改为调用 `Observation::refreshOptimizationProperties` |
| D3 | `2b629572` | “单下标、单字节步长的 GEP，其下标就是字节偏移”的匹配收成 `getByteOffsetIndex`，`AbstractAccess::fromPointerAndSize` 和 LPOC 共用 |
| D2 | `c266f536` | 循环出口目标的解析收进 `MAIRLoopInfo::getExitSite` 和 `findExitPoint`，LIOM 和 LPOC 共用；多个程序点匹配时选中的仍是原来那个 |
| D5 | `5d3579df` | `MAIRScalarExpr` 的定义挪到 `AbstractAccess` 之前，区间大小表达式直接以 `MAIRScalarExpr` 存取（`setSizeExpr`/`getSizeExpr`），去掉 `void*` 入参、`fromNode` 和自由函数 `expressionSize` |
| D1a | `ebc80b62` | `MAIRGraphTest.cpp` 和 `MAIRMaterializationTest.cpp` 不再各带一份模块解析、建图和分析注册，改用 `MAIRPassTestUtil.h`；`buildGraph` 增加 barrier trait 模板参数 |
| D1b | `6f650f17` | `MAIRGraphTest.cpp` 里 52 个按 pass 归类的测试移到各 pass 的测试文件，套件名和测试名不变；与留下的图测试共用的 helper 移到 `MAIRPassTestUtil.h`。`MAIRGraphTest.cpp` 从 6208 行降到 3896 行 |

验证：

- D2–D5 每项提交前都跑 `check-mair` 和 546 组 `mair-opt` 逐字节差分，全部 0 处不同；参考二进制是第四轮开始前的构建。最后一个提交上又跑了一遍差分，仍是 0 处不同。
- D1a、D1b 只动测试。测试二进制列出的 210 个测试名在搬迁前后是同一个集合；用 `check_tests.py` 逐个比较了搬迁前后每个 `TEST` 块的内容，忽略空白后全部相同。
- D5 改了约 20 处测试调用（`X.setSizeExprNode(E.getOpaqueNode())` 改为 `X.setSizeExpr(E)`），是机械替换。

局限：

- D2 的 `findExitPoint` 仍是在父图里线性查找，每个被移动的 observation 查一次。这是原有行为，没有测过它是否成为热点。
- D1b 之后 `MAIRPassTestUtil.h` 里的 helper 是在全局命名空间里的 `inline` 函数，原来它们在匿名命名空间里。

与另一条分支的关系：第一轮的分支 `claude/mair-review-refactor` 在本分支分叉之后又有了 3 个提交（`6844880d`、`46dc514b`、`9af0f6ad`，修改循环提升判据、PostDom 边和 barrier 计数）。本分支没有包含它们。试合并显示 3 个文件冲突：`MAIR.cpp`（本分支已把它拆成四个文件）和两个 pass 测试文件（本分支往里搬了测试）。两条分支不能再快进合并，需要手工解决冲突。

第四轮审查里列出、这一轮没有做的：

- ASan 和 TSan 接入层的重复（两个几乎相同的 backend 类、`CanInstrumentViaMAIR` 的相同前半段）。
- `MAIR.h` 末尾的 10 个 `GraphTraits` 特化，生产代码只用到一个。
- 没有生产调用方的公开接口；只有测试在用的“图拥有分析结果”的 `build` 重载。
- `MAIROptimizationConfig` 里恒为 `true` 的 `AllowRelocation`，以及完全相同的 `generic()` 和 `forThreadSanitizer()`。
- `RedundantObservationAnalysis` 注册成分析却从不经分析管理器查询。
- pass 的注册名和统计标签是两套名字。
- `RedundantObservationEliminationPass.cpp` 里注释说被钉住的 observation 可以作见证者，代码却把它跳过了；这是注释与实现不一致，不是重构。

任务资料在 worktree 的 `.archive/mair_dedup_refactor_20261112_1705/`，被 `.gitignore` 忽略。

## 9. 审查发现的回归及修正

另一位 Agent 审查了 `1482b67a..60bdfd89`（报告在主检出目录的 `.archive/claude_refactor_review_20261112_1755/review.md`），确认一项 P2 性能回归，没有发现新增的正确性缺陷。回归属实，已修正。

- 问题：M3（`280ef2c9`）把六个 pass 的输入计数 `MAIRStat::compute(G)` 挪到了统计开关判断之前。在 Release 构建（`NDEBUG`，LLVM 统计被编译掉）且不传 `-mair-stats` 时，没有任何地方使用这个计数，但每次 pass 访问图仍会遍历全部程序点。M3 之前这次遍历在 `#if LLVM_ENABLE_STATS` 和 `mairStatsEnabled()` 之后。
- 我当时的错误：认为 `NoopStatistic` 是空操作，所以可以去掉条件编译。空操作的是 `+=` 本身，它的参数仍然会被求值。
- 为什么差分没有发现：546 组差分比较的是输出，这个回归只多做了无用功，不改变输出；而且差分用的是 Debug 构建，那里 LLVM 统计本来就是开着的。
- 修正（`4d1d46f9`）：`MAIRStatScope` 的构造函数改为接收 LLVM 统计量和一个计数回调，只有在 LLVM 统计被编译进来或 `-mair-stats` 打开时才调用回调，两处共用同一次计数。六个 pass 的调用点仍是一条语句。`mairStatsEnabled()` 重新公开。

验证：

- 用 `-O2 -DNDEBUG` 编译六个 pass 并检查 IR（`check_release_ir.py`）：修正前的 LIOM 在入口块无条件调用 `compute`；修正后六个 pass 都先查询 `mairStatsEnabled`，入口块里没有 `compute`。
- 另配了一个 Release 构建目录：修正前后的 `mair-opt` 在 546 组输入上输出逐字节一致（含 `-mair-stats`）；Debug 构建同样一致。
- 新增单元测试 `MAIRStatLog.CountsPassInputsOnlyWhenStatisticsConsumeThem`，在 Debug 和 Release 下各跑过：统计被编译掉且未开 `-mair-stats` 时回调不被调用。两种构建下 211 项单元测试全部通过，lit 27 通过、1 unsupported。

局限：

- 没有测量这次遍历实际花多少时间，所以不知道回归有多大，也不知道修正省了多少。
- 新测试没有覆盖“`-mair-stats` 打开时计数被记录”这一支；那一支由现有的 `mair-stats*.ll` lit 用例和 546 组差分覆盖。

审查报告里另外两条记录我没有改动：

- M6 之后 `Observation::fromInstruction(I, DL)` 对 load/store/atomic 用的是指令所属模块的 DataLayout，并要求指令已挂在函数里，传入的 `DL` 参数对这几类指令不再起作用。仓库里的调用方传的就是模块的 DataLayout，行为一致，但这个公开接口对任意输入不再等价。
- N2 值得补一个“符号窄位先加后扩”的专项测试。

任务资料在 worktree 的 `.archive/mair_stat_gating_fix_20261112_1810/`，被 `.gitignore` 忽略。

## 10. 按 review 意见返工（2026-11-12）

第一轮对 F2、F3 的修法过于保守（F2 直接不建跨迭代的 PostDom 边，F3 把不可约环的块整体钉住）。本轮改为根因修复，并回答了 F1 和 ASan 配置的两个问题。任务资料在 `.archive/mair_review_followup_20261112_1607/`。

最终状态：`ninja -C build check-mair` 通过，216 项单元测试，lit 27 通过、1 unsupported。ASan 全量 lit（带 MAIR）529 项：325 通过、198 unsupported、1 XFAIL、5 XPASS；这 5 个 XPASS 在改动前的基线里就存在，除此之外没有失败。TSan 全量 lit 381 项：291 通过、89 unsupported、1 XFAIL（在 `c3a267c1` 上跑的，其后的 `15862f21` 只改 ASan，之后只重跑了 5 项 `mair-*`）。MSan、XSan 没有运行。

### F1：后支配 preheader 的条件确实多余（`6844880d`）

- “来源块支配每个 exiting block”蕴含“来源块后支配 preheader”：从 preheader 到返回的每条路径都要经过某个 exiting block。因此删掉了后支配树查询。
- 唯一的例外是没有出口的循环，此时前一个条件为空真，而旧的后支配条件实际起了作用（差分发现 `loop-hierarchy.ll` 的 `@no_exit` 行为变了）。改为要求来源块支配每个 latch，并补了测试 `HoistsFromALoopWithoutExitsOnlyWhatEveryIterationRuns`。改后与前一提交逐字节比较 378 组一致。
- 例子和推理写在 `MAIRLoopInfo::canHoistToPreheader` 的注释里，测试是 `DoesNotHoistAnAccessTheLoopCanExitWithout`。
- 没有实现的部分：带“S 执行过”indicator 的外提。单出口循环已有带 guard 的下沉可用；例子里的多出口循环需要在每条出口边放带 guard 的副本并改写 CFG，只记了 TODO。

### F2 与 F3：PostDom 边记录它迭代的环（`46dc514b`）

- 两个 bug 是同一个问题：后支配者可能要等某个环开始新一轮迭代之后才到达，而环内定义的值那时已经变了。
- 做法：图持有 LLVM `CycleInfo`；每条 PostDom 边记录“两端之间可能开始新一轮迭代的最外层环”，判据是存在一条路径在到达后支配者之前进入该环的 header 或离开该环。沿这种边，只有在该环内不变的 observation 才能被关联。地址不随循环变化时照常按 PostDom 优化，地址随迭代变化时才关闭。
- 自然循环和不可约环走同一条规则。不可约环的块不再被钉住，留在外围层按普通规则优化；它没有 preheader、唯一 header 和 trip count，LIOM/LPOC 不适用，记了 TODO。
- 冗余分析按“对该 observation 关闭的环集合”各算一份可达性；公共 observation 下沉遵守同一规则。
- 例子和推理写在 `iteratedCycleBetween` 的注释里。测试：`KeepsAccessCoveredOnlyInALaterIteration`（地址随迭代变，保留）、`EliminatesInvariantAccessCoveredInALaterIteration`（地址不变，照常删除）、`IrreducibleCyclesAreOptimizedWithinOneIteration`、`test/MAIR/TestCases/loop-irreducible.ll`。
- 与前一提交比较 378 组：保留的 observation 只在不可约环用例上变化（`loop-irreducible.ll` 7 条变 4 条，`irreducible_hole.ll` 3 条变 1 条，都是地址不变的访问重新被删除），其余差异只是边列表多了 `iter` 标注。jsoncpp 在 `-O1` 下保留的 observation 数不变。
- 没有实现的部分：把例子里两处写上提到共同前驱再由周期合并移出循环，记了 TODO（需要公共 observation 上提能越过空块）。

### 新发现：重复执行的端点块里的 barrier 没被计入（`9af0f6ad`）

- `hasBarrierPath` 跳过两端的块。被覆盖的一端所在块如果能在不经过见证块的情况下再次执行，它块内的 barrier 就落在见证和被覆盖访问之间。循环 header 开头的调用就是这样，夹在 header 里较早的写和后支配它的写之间。
- 这个问题在最初的基线里就有；第一轮 F2 去掉跨迭代的边时把后向的情形一并挡住了，本轮恢复这些边后由随机测试重新暴露。前向的情形（不可约环）与 F2 无关。
- 测试：`CountsBarriersOfAnEndpointThatRunsAgain`。

### 随机化健全性测试（`561e382e`）

- `RandomizedSoundnessTest.cpp` 生成带嵌套循环和不可约环的小函数，随机执行，检查两条性质：每次写都有同地址、中间无 barrier 的 observation；每个 observation 都有这样的写。
- 做过变异检查：分别关掉环不变性规则、上一条 barrier 规则、外提的出口支配规则，测试都会失败。
- 它不覆盖 LPOC、LocationMerge、区间和大小的推理，也不覆盖物化。

### ASan 为什么不用 PostDom 覆盖（`c3a267c1`、`15862f21`）

- 来历：`DomCoverageOnly` 是 2026-07-17 的 `3531db9a` 引入的，目的是保证检查在访问之前（后面的检查不能顶替前面的）。2026-11-10 的 `64af7808` 放开了 ASan 的 observation 移动，图收缩本来就会把检查沉到访问之后，`mair-check-order.cpp` 也已明确接受这一点。所以这个限制已经保护不了任何顺序，只是白白多留检查，结论与你的判断一致。
- 改动：删掉这个开关，所有配置都沿两种边传播覆盖。jsoncpp 三个翻译单元在 `-O1` 下的 ASan 检查数：不开 MAIR 为 2361/1230/1022，原配置 2214/1176/994，打开 PostDom 覆盖后 2192/1160/981。
- 调查中发现一个真实问题：ASan 的反向 barrier 只包括“可能写内存的调用”，所以只读但可能不返回的调用不算 barrier，访问的唯一检查可能落到这种调用之后而永远不执行。这在打开 PostDom 覆盖之前就存在（图收缩同样会造成）。`15862f21` 把任何不保证把控制流交给后继的指令都算作 ASan 的反向 barrier，测试是 `test/asan/TestCases/mair-may-not-return.c`；jsoncpp 的检查数不受影响。
- 仍然存在、且不是本轮引入的限制：检查落在访问之后时，足够野的访问会先触发 SEGV 而不是 ASan 报告（`high-address-dereference.c` 因此在 MAIR 下标为 unsupported）。
