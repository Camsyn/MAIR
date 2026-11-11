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

- 复现：`-passes=liom liom_outer_backedge.ll`。内层循环里的 `store %p` 只在 `%j > 5` 时执行，却被提升到内层 preheader 且使用 `AlwaysGuard`，于是 `%j <= 5` 的每轮外层迭代都会观测一个程序从未访问的地址。
- 影响：所有配置。ASan 表现为误报，TSan 表现为虚假的竞争访问。
- 根因：`MAIRLoopInfo::canHoistToPreheader`（`Analysis/LoopAnalysis.cpp`）用 `PDT->dominates(Source, Preheader)` 证明“进入循环必执行”。后支配只保证“最终会执行”，这条路径可以先离开内层循环、绕外层回边再回来。
- 修复：增加必要条件“Source 所在块支配该循环的每个 exiting block”（`Loop::getExitingBlocks` + `DominatorTree::dominates`）。这与 LICM 的 `allLoopPathsLeadToBlock` 是同一判据；不直接用 `LoopSafetyInfo::isGuaranteedToExecute`，因为它会把任何可能抛异常的调用都当作阻断，接受域明显变窄，而这类阻断在 MAIR 里由 barrier trait 负责。
- 不损失合法提升：若 Source 不支配某个 exiting block，就存在一条进入循环后不经过 Source 直接离开的路径，原提升本来就不成立。

### F2 PostDom 冗余消除跨越循环回边（漏报）

- 复现：`-passes=roe pdom_backedge.ll`。块 A 在 `%i < 3` 时写 `base[i]`，其 observation 被块 B 的同名 `%p` 写“覆盖”并删除；但 B 只有绕回边进入下一轮迭代才会执行，那时 `%p` 已是另一个地址。`base[0..2]` 的写不再被观测。
- 影响：允许 PostDom 覆盖的配置，即 generic 与 TSan。ASan 配置为 `DomCoverageOnly`，不受影响。
- 根因：循环作用域图里的 PostDom 边按整个函数的后支配树建立（`MAIRGraph::build`，`MAIR.cpp`）。“B 后支配 A”可以只通过回边成立，而循环内定义的 SSA 值跨迭代不是同一个值。现有的 `sameLoopForPostDom` 只挡住了“循环内与循环外”这一种情形。Dom 边不存在这个问题（定义点支配两个使用点时，最后一次定义之后必然先经过支配者）。
- 修复：在建图时只为“迭代内”的后支配关系建 PostDom 边。对循环 L 内的块 T 及其（同作用域的）后支配者 F，若存在一条从 T 出发、在到达 F 之前就回到 L 的 header 或离开 L 的路径，则不建这条边；F 就是 header 时同样不建。检查是限制在 L 内的一次 DFS，量级与已有的 `hasBwdBarrierPath` 相同。
- 取舍：这是保守做法，循环不变地址“绕回边被覆盖”的机会会丢失；这类 observation 本来由 Dom 覆盖和 LIOM 处理。选择在建图处修而不是在 ROE 里再加一个使用点判断，是为了让“循环作用域图描述一次迭代”成为图本身的不变量。
- 连带影响：含循环的 lit golden 中 `preds/succs` 列表会变化，用 `utils/update_mair_test_checks.py` 重新生成并逐项核对。

### F3 不可约环的块识别不完整（漏报）

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
- “值在某点可用”有三份实现（`MAIRLoopInfo::isValueAvailableAt`、COM 的 `isValueAvailableAt`、物化的 `valueAvailableAt`），对非指令值的处理略有不同，可以统一到 `DominatorTree::dominates(const Value*, const Instruction*)`，但要先确认三处接受域一致。
- `addressSCEV`（`MAIR.cpp`）与 `regionAddressSCEV`（LPOC）几乎相同，区别是零偏移时用 `getUnknown(Base)` 还是 `getSCEV(Base)`；上一轮审查已指出两者不等价，合并前需要先定语义。
- 仅被单元测试使用的接口：`MAIRGraph::isAncestor`、`MAIRLoopInfo::getLoopFor`、`MAIRLoopInfo::getTopInvariantLoop`。是否删除由维护者决定。
- `MAIRGraph::getLoopGraph` 每次调用都线性遍历整棵作用域树，而 LIOM/LPOC 的调用方查询的总是当前图自己的循环，可以直接取 `getLoopPreheader()`。
- min/max 表达式用 `icmp`+`select` 降级，可改用 `llvm.smin` 等 intrinsic；这会改变产出的 IR 文本和现有测试期望，不属于等价重构。
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
