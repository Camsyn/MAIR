# MAIR 循环层次

## 问题与约束

函数入口的一次检查不能代表循环中的每一轮访问。例如，循环内先读 `p`，再执行 `free(p)`，下一轮读 `p` 必须重新检查。单层 MAIR 将循环外的支配关系连到循环内后，普通冗余删除可能错误复用入口检查；只查询首次进入循环的路径不能证明后续迭代安全。

MAIR 用循环层次限制普通优化的范围。每个函数有一个根图，每个 LLVM 自然循环有一个独立 body 图。普通边只连接同一图的 ProgramPoint。循环专用优化在满足条件时，才把 Observation 从 body 导出到父图。Observation、MemoryRegion、Site 和 sanitizer backend 的职责保持原有划分。

## 图和循环边界

每层图只包含该层直接拥有的 LLVM 基本块与 ProgramPoint；子循环的块属于子图。父图通过 `LoopProgramPoint` 关联子图，其两种边界为：

| 边界 | 物理位置 | 数量 |
| --- | --- | --- |
| `LoopPreheader` | 循环真实 preheader 的 `BBExit` | 每个循环一个 |
| `LoopExit` | 循环 dedicated exit 的 `BBEntry` | 每个不同出口块一个；无出口时为零 |

循环边界是额外的语义节点，和所在基本块原有的 entry/exit 代表点分别保存。同一循环的全部边界关联同一个 body，body 由父图唯一拥有。嵌套循环逐层拥有，ProgramPoint 名称在函数整棵图中唯一。

`begin/end/size` 仍表示本层。`graphs()` 和 `allProgramPoints()` 显式遍历整棵图，供统计、验证、dump 和物化使用。普通 Dom/PostDom 边、祖先查询和图收缩不能跨层；图简化保留循环边界。父图中循环前后的点仍在同一层，是否能关联仍须通过原有 CFG barrier 判断，不能因为封装了子循环就忽略循环里的 barrier。

## 输入与优化规则

构图前使用 LLVM `LoopSimplifyPass` 建立真实 preheader 和 dedicated exits，再取得构图依赖的分析。构图本身不修改 LLVM IR。直接调用构图 API 的调用者也必须满足这些前提。不能形成这些边界时直接报告错误，不创建假边界。不可归约环不是自然循环，`LoopInfo` 不表示它，所以它没有自己的循环层，环里的块留在外围那一层，按普通规则优化；不变外提和周期合并不适用（没有 preheader、唯一 header 和 trip count）。TODO：如果这类环有价值，再探索为它建层。

### PostDom 边与环的迭代

构图时用 LLVM `CycleInfo`（自然循环和不可归约环的统一表示）给每条 PostDom 边记录“两端之间可能开始新一轮迭代的最外层环”（`ProgramPointEdge::getIteratedCycle`，由 `iteratedCycleBetween` 计算，`src/instrumentation/MAIR/MAIRGraphBuilder.cpp`）。从被后支配的块出发，只要有一条路径在到达后支配者之前进入了某个公共环的 header，或者离开了该环，这个环就算被迭代。

原因见下例：`B` 后支配 `A`（函数只从 `B` 返回），但 `%i < 3` 时控制流从 `A` 绕回边回到 `H` 才到 `B`，此时 `%p` 已是另一个地址。把 `A` 的 Observation 当作被 `B` 覆盖而删掉，`base[0..2]` 的写就没人观测。

```llvm
H:                                  ; %p = &base[%i]
  br i1 (%i < 3), label %X, label %B
X:
  br label %A
B:
  store i32 1, ptr %p               ; i >= 3 时写 base[i]
  br i1 %done, label %exit, label %A
A:
  store i32 2, ptr %p               ; 每轮都写 base[i]
  br label %H
```

规则：沿一条记录了环的 PostDom 边，只有在该环内不变的 Observation（区域引用的值都不在环内定义，`Observation::isInvariantIn`）才能被关联。地址不随环变化时，绕回边成立的后支配和普通后支配一样可用；地址随迭代变化时这条边对它关闭。冗余删除和公共 Observation 下沉都遵守这条规则（`ProgramPointEdge::isOpenTo`）。Dom 边不需要：若 `W` 支配 `T`，那么 `T` 之前最后一次定义某个值之后，控制流必然又经过了 `W`。

TODO：上例两处写可以先提到 `H`（每轮必执行其一），再由周期合并移出循环；这需要公共 Observation 上提能越过空块 `X`。

### 终止假设

MAIR 假设环一定会终止，即后支配者最终会被执行到。沿 PostDom 边的覆盖和下沉到循环出口都建立在这条假设上，对所有 sanitizer 一样（约定写在 `PPEdgeKind` 的注释里，`src/instrumentation/MAIR/MAIR.h`）。下例中 `witness` 后支配 `header`，`%late` 的 Observation 覆盖 `%early` 的；如果 `%repeat` 一直为真，控制流永远停在 `header`，`%early` 就没有被观测。这是已接受的行为，不做终止性证明。

```llvm
header:
  %early = load i32, ptr %p
  br i1 %repeat, label %header, label %witness
witness:
  %late = load i32, ptr %p         ; 后支配 header
  br i1 %finish, label %exit, label %header
```

假设只针对环。单条可能不返回的指令（可能 `exit` 或抛异常的调用）不在假设之内，仍由各 sanitizer 的 barrier trait 处理。

barrier 也要考虑重复执行：被覆盖的一端所在块如果能在不经过见证块的情况下再次执行，它自己块内的 barrier 就落在见证和被覆盖访问之间，同样阻断这条边（`MAIRBarrierAnalysisCore::hasBarrierPath`，`src/instrumentation/MAIR/MAIRBarrierAnalysis.cpp`）。

每个优化阶段按子图优先顺序执行，随后处理父图。普通冗余删除、位置合并和公共 Observation 移动继续使用本层 MAIR。它们不能沿父子图链接复用检查。循环不变量移动和周期合并可以显式导出到父图中的循环边界，嵌套导出每次只跨一层，逐层重新证明。内层周期合并会把新的区间观察放到父图的循环出口，而父层第一次不变外提此时已经结束。`default<Og>` 和 `default<O1>` 都在周期合并后再跑一次不变外提，让外层循环重新证明这条观察。

不变量移动按本图 ProgramPoint 和 Observation 的原顺序逐项计划并立即提交，每次实时查找父图目标。提交只删除当前源 Observation 并更新父图，不改本图其他候选、拓扑或 LLVM 分析，因此不需要从头重扫；若以后改变这些前提，必须重新检查单遍遍历的等价性。

`default<O1>` 在首次不变量移动前先做冗余删除。循环展开可能在同一条件块中留下多次相同访问；先消除重复观察，才能在下沉时只保留一个来源 guard。若先逐项下沉，来源不同会生成不同 guarded ProgramPoint，随后冗余分析也不会把它们作为无条件覆盖来源。冗余删除只保留原有覆盖见证及其来源，不拼接被删除观察的 provenance，因此不改变保留检查的源位置规则。消冗余仍遵守既有层次、覆盖和 sanitizer 规则。`default<Og>` 不改变既有优化顺序。

两个 Observation 互相覆盖时（层次相同，例如 ASan 下的读和写，或 TSan 下的两个读；`A` 支配 `B` 且 `B` 后支配 `A`），保留先执行的 `A`，删掉 `B`，与读写无关。层次不同时只有高层次的一方能覆盖，规则不变：泛型配置和 TSan 下，先读后写仍保留写。跨 ProgramPoint 消冗余按 Dom 边的后序访问 ProgramPoint，即每个 ProgramPoint 都先于沿 Dom 边能到达它的 ProgramPoint 被检查；先检查的被覆盖者删除后不再作为见证，所以支配者留下（`doInterROE`，`src/instrumentation/MAIR/Transforms/RedundantObservationEliminationPass.cpp`）。图的链表顺序跟随基本块布局，不一定是支配顺序，不能直接用。顺序只决定互相覆盖的两者谁留下，不影响正确性：被删除的 Observation 在被删时总有一个尚未被删除的见证（`ExcludedSources`），与访问顺序无关。同一 ProgramPoint 内的消冗余按稳定排序保留链表中靠前的一个；`default<O1>` 的移动 pass 会把区域和层次相同的 Observation 合并（保留全部来源），只有不合并的 `default<Og>` 才会在同一 ProgramPoint 留下多个，顺序是循环子图的链表顺序。测试：`test/MAIR/TestCases/redundant-observation-elimination.ll` 的 `earlier_*`、`later_write_covers_earlier_read`、`dominator_laid_out_later`，单元测试 `AddressSanitizerKeepsTheDominatorOfMutuallyCoveringAccesses`、`ThreadSanitizerKeepsTheLaterWriteOverADominatingRead`。

周期合并也直接按本图 ProgramPoint 的原顺序遍历。聚合目标属于父图，提交不增删本图 ProgramPoint，因此不需要快照；内层仍先递增迭代器再删除当前 Observation。来源指针查询使用 LLVM 的 `getLoadStorePointerOperand`，仅接受 load/store，并保留空指针检查。

优化输入统计按每次本层 pass 调用累计；Observation 导出后再次作为父层输入，会再次计入该统计，这不表示重复物化。`print<stat>` 则汇总当前整树的 ProgramPoint 和 Observation 数量。

宿主 LLVM 的 `-stats` 在 Release 构建里不会打印。要看每个 pass 实际改了什么，用 `-mair-stats`。`xclang -mair-stats` 会把它转成 `-mllvm -mair-stats`；`mair-opt -mair-stats` 直接认这个选项。每个编译单元在 stderr 打一块汇总，不按函数展开，避免 C++ mangled 名字淹没构建日志。`default<O1>` / `default<Og>` 里重复出现的 pass 带 `#1`、`#2`，所以第二次不变外提不会和第一次混在一起。`in` 仍按每次本层调用累计；比较效果看 `preheader`、`exit`、`range`、`periodic`、`merged`、`intra`、`inter`、`hoisted`、`sunk`。

sanitizer 编译还会多一行 `select`，在优化之后统计 lowering 去向：`in` 是非 call-like 的终态 Observation，`lowered` 交给 MAIR 物化，`native` 退回 sanitizer 自己逐访问插桩；`multi`（多源，合并产物）、`symbolic`（非常量偏移/大小或非连续区间，含 periodic）、`vector`、`shared`（来源同时被一个退回的 Observation 持有）说明退回原因，其余退回原因是 sanitizer 自身判定（如 ASan 安全栈访问、TSan 未逃逸 alloca）。`fallback` 是退回 native 的来源指令数。实现在 `selectMaterializableObservations`（`src/instrumentation/MAIR/Materialization/MAIRMaterialization.cpp`），测试 `test/MAIR/TestCases/asan/mair-select-stats.c`。这几个原因字段只描述 Observation 的形状；ASan 已能 lowering 多源、符号区间、periodic 和固定宽度向量（见“ASan 的 lowering”），在 ASan 下它们只剩 masked 区域和来源本身被 ASan 拒绝的情况。

统计汇总按函数首次出现的顺序遍历各函数的 pass visits，以 pass 标签首次出现的位置决定行顺序；不是全局事件时间顺序。汇总阶段一次累计，保持 ordinal、字段顺序及只含 `in` 的行，全零行不打印。

循环 barrier 摘要来自构图时的 LLVM 指令，并包含子循环。导出 Observation 不会抹掉摘要。当前最小规则是：提升、下沉和周期合并都要求循环中正向、反向均无 barrier。任一迭代中的 barrier 都阻止相应导出，不需要逐动态回边路径查询。这是保守条件，并未放宽既有必执行、SSA 可用性、精确计数、affine recurrence 或 guard 条件。提升到 preheader 的必执行条件是：来源块支配循环的每个 exiting block，即任何出口都只能在来源块执行之后走到（`MAIRLoopInfo::canHoistToPreheader`，`src/instrumentation/MAIR/Analysis/LoopAnalysis.cpp`，例子写在该函数的注释里）。这个条件蕴含“来源块后支配 preheader”，所以不再单独查后支配树；反过来不成立，控制流可以不经过来源块就离开内层循环，再从外层循环绕回来。没有出口的循环改为要求来源块支配每个 latch。不满足条件的访问仍可用“来源执行过”的 guard 下沉到唯一出口；多出口循环需要在每条出口边放带 guard 的副本并改写 CFG，尚未实现。ASan 允许搬动普通 Observation，检查可以落在实际访问之后，因此也和 TSan 一样沿 PostDom 边传播覆盖。前提是后面的检查一定会执行：ASan 的反向 barrier 除了可能写内存的调用，还包括任何不保证把控制流交给后继的指令（可能抛异常或不返回的调用）。原子访问仍然不能搬。

带“来源访问执行过”条件的下沉，以本次循环调用的 preheader 为 guard 重置位置。嵌套循环的下一次调用不会继承上一次调用的 true 状态。整树统一生成物化计划，统一 prepare/apply；完成后整树都进入 consumed 状态。

物化必须在 sanitizer emission 前完成全部拆边、guard 和观察参数 lowering。sanitizer 可自行拆 CFG，MAIR 无法同步维护这些修改后的局部分析，因此不能交错执行后续 guard 更新。第一阶段保留 DTU/LI，pending record 自有 lowered observations，并保存 plan unit 指针和稳定指令插入点；第二阶段按原顺序从指令的当前基本块重建 builder，仅在同步 emit 时构造 ArrayRef。若后来的 guard 拆分共享 anchor，早先 pending 的插入点改指向拆分前块的新条件 terminator，保持原先 emission 在该 guard 之前。backend 必须保留后续插入指令和已物化参数；第一遍之后不再消费局部 CFG 分析。

物化复用本次 preflight 建立的 DT/LI，通过 LLVM 拆边接口增量维护；不建立未被查询的 PDT。backend 的 prepare 必须保持 IR 不变，失败时不能留下 CFG 修改。

MAOpt 的调用者通过 `allProgramPoints()` 读取当前图，不维护额外的 Observation 快照。sanitizer 在优化前记录来源集合；ASan 先按读写开关过滤，再记录来源。优化后先收齐不能由 MAIR 物化的来源，再排除存在同源冲突的候选。已处理来源是优化前集合减去原生路径来源，不能只取最终候选的来源，否则被冗余删除的检查会重新插入。call-like Observation 保持原生 lowering。

内层范围起点复用已有 LLVM Value 时，除 SCEV 相等和目标位置可用外，还必须保证类型匹配与 poison 安全。指令候选只有在 `ScalarEvolution::canReuseInstruction` 允许复用且无需丢弃 poison flags 时才接受；优化阶段不修改原 LLVM IR。`loop-start-reuse.ll` 覆盖不安全候选、非 SCEV 类型以及继续寻找安全候选。

## `default<O1>` 的顺序

`buildMAIRGraphO1Pipeline`（`src/instrumentation/MAIR/MAIRPasses.cpp`）依次运行：

```
simplifygraph#1 → roe#1 → locationmerge#1 → liom#1 → lpoc → liom#2 → locationmerge#2 → roe#2 → simplifygraph#2
```

每个 pass 各自做一次整树遍历（子图先于父图）。顺序依据 e2e 的 `-mair-stats` 和 `instrumented_mops`：

- `roe#1` 在最前，原因见上文（下沉时只保留一个来源 guard）。把位置合并提到它前面试过：re2 ASan 少 81 个检查，但 7zip ASan 多 112、sqlite3 ASan 多 8；合并还会拼接来源，ASan 因多源退回 native 的 Observation 变多（sqlite3 `multi` 342 → 463），所以不采用。
- `locationmerge#1` 在不变外提和周期合并之前。循环体里的 `p[2i]`、`p[2i+1]` 先合并成一个 8 字节、步长也是 8 的访问，周期合并得到一个连续区间；先做周期合并只能得到两个 stride 8、size 4 的 periodic 区域。7zip ASan 中周期合并的 `range` 从 125 增至 438，`periodic` 从 3282 降至 1060。测试：`test/MAIR/TestCases/o1-pipeline.ll` 的 `o1_location_merge_before_periodic_combine`。
- `liom#2` 原因见上文；新顺序下 7zip ASan 仍有 12 个提升。
- `locationmerge#2` 合并外提和周期合并放到循环边界的 Observation，包括足迹相接的 periodic 区域（见“地址合并”）；`roe#2` 删除被合并后区域覆盖的访问（7zip ASan 110 个）。
- 公共 Observation 移动（COM）、第三次位置合并和第三次冗余删除已移出：调整前后在 4 个项目 × 2 个 sanitizer 中都是 0。COM 不触发是因为 `-O2` 的 SimplifyCFG 已经把两条分支里相同的访问提到或沉到公共块（例如两边都读 `p[0]` 时，读已在入口块）。COM 仍可用 `-passes=com` 单独运行。
- 位置合并之后再跑一轮 `lpoc`/`liom` 没有收益（jsoncpp、re2、sqlite3 的检查数不变），所以只跑一轮。

e2e 的 `instrumented_mops`（MAIR 变体，oracle 全部通过；无 MAIR → 旧 O1 → 新 O1，新 O1 含 periodic 足迹合并）：

| 项目 | ASan | TSan |
| --- | --- | --- |
| jsoncpp | 5461 → 4573 → 4570 | 6732 → 5644 → 5644 |
| re2 | 35194 → 30071 → 29789 | 38208 → 32552 → 32496 |
| sqlite3 | 36971 → 35485 → 35424 | 39274 → 36825 → 36783 |
| 7zip | 112703 → 98218 → 96579 | 117691 → 106541 → 105462 |

调顺序不需要重编：隐藏选项 `-mair-pipeline=<text>`（`xclang ... -mllvm -mair-pipeline=...`）用 `mair-opt -passes` 的语法替换 sanitizer 跑的流水线（`buildMAIRGraphDefaultPipeline`）。`NAME#N` 给重复的 pass 编号，`-mair-stats` 分开打印（测试 `mair-stats-counts.ll`）。

## 地址合并

符号地址只有在能够证明字节差时才合并。有 ScalarEvolution 时使用地址 SCEV 的常量差；证明失败即保留原观察。没有该分析时，只用 LLVM 的常量偏移剥离匹配相同剩余地址，以 APInt 计算差值，不把整数 `add` 当成不回绕的地址距离。

两个 periodic 区域在基址、stride、trip count 都相同、access size 为常量、起点只差常量 `d`（不大于较低一方的 size）时合并成一个：每一轮里两段足迹相接或重叠，并集是一段 `[较低起点, max(a, d + b))`，所以结果保留较低起点、stride 和 trip count，只把 size 放大，正好是并集（`PeriodicRegion::merge`，`src/instrumentation/MAIR/MAIRRegion.cpp`）。起点按“根指针 + 常量 + 符号项”比较，偏移类型必须和指针索引同宽，常量差才精确。有空隙、stride 或 trip count 不同都不合并。合并后若 stride 不大于 size，区域变成连续的，TSan 用一次 range 访问记录，而不是逐轮调用 `__tsan_{read,write}_periodic`。测试：`test/MAIR/TestCases/locationmerge-periodic.ll`。

有空隙时还有一种精确合并：交错。m 个 periodic 区域基址、常量 access size `a`、trip count `T` 和常量 stride `S` 都相同，起点排序后等距 `g = |S| / m`（`m * g == |S|`），这是向量化/交错展开后常见的形状（例如 stride 32 的 +0 与 +16 两路 gather）。取 `s0` 为最低起点（`S < 0` 时为最高起点）、`d = S / m`，各起点为 `s0 + k * d`（`0 <= k < m`），第 k 个区域的第 i 轮访问 `s0 + k * d + i * S = s0 + (i * m + k) * d`，恰是起点 `s0`、stride `d`、access size `a`、trip count `m * T` 的区域的第 `i * m + k` 轮；反过来该区域的每一轮都唯一对应某个 (i, k)。所以并集正好是一个 periodic 区域，`g > a`（轮间有空隙）和 `g < a`（重叠）都精确，每一轮仍单独检查，空隙不查（`PeriodicRegion::mergeInterleaved`）。trip count 先零扩展 `ceil(log2 m)` 位再乘 m（`(2 * zext(T to i66))`），乘积不会回绕；常量 trip count 直接折叠。materialization 已支持 `Mul`/`ZeroExtend`，ASan/TSan 本来就把 trip count `ZExtOrTrunc` 到指针宽度，后端无需改动。

成员不能两两拼出：m = 3 时没有两条区域相距 `S / 2`，所以 LocationMerge 显式分组（`mergeInterleavedRange`，`src/instrumentation/MAIR/Transforms/LocationMergePass.cpp`）：periodic 类先按上段做两两相接合并，再对每个区域找候选（`PeriodicRegion::getInterleaveDistance`：基址、stride、size、trip count 相同，起点差为常量且 `0 < |d| < |S|`），从小到大尝试候选间距 g（m 尽量大），要求 `s0 + k * g` 全部在场；组由列表中最靠前的成员吸收其余成员，来源按列表顺序追加。合并后的区域 stride 变小，会再试一次。间距不整除 stride、`m * g != |S|`、size 或 trip count 不同都不合并。测试：`test/MAIR/TestCases/locationmerge-interleaved.ll`，运行时 `test/MAIR/TestCases/asan/mair-interleaved-runtime.c`（越界仍报告、界内与只 poison 空隙时不报告）。

Observation 合并保留完整来源指令序列及重复项。若右项严格包含左项，右项来源放在前面；相等、左项包含或非包含的区间并集仍按左后右追加。首项及首个有效 debug location 依赖这个顺序，不能把来源改成集合。

## ASan 的 lowering

ASan 按 Observation 的区域检查，而不是按来源指令逐个检查（`XsanAddressSanitizer::instrumentMAIRObservation` 和 `instrumentFunctionViaMAIR` 里的 `CanInstrumentViaMAIR`，`src/instrumentation/llvm23/AddressSanitizer.cpp`）。接受条件：每个来源都是 native ASan 也会检查的 load/store（`getInterestingMemoryOperands` 恰好给出一个操作数、不被安全栈/全局访问优化省掉、不是 scalable 向量）；区域是大小已知的连续区间（偏移可以是符号，大小可以是 trip count 表达式），或 periodic 区域。任一来源不满足时整个 Observation 退回 native，由 native 自己决定每个来源查不查。

native ASan 永远不查的 load/store（`getInterestingMemoryOperands` 为空：读写开关关闭、promotable 或 stack-safety 证明安全的 alloca 等；或被安全栈/全局访问优化省掉）在优化**之前**就从图中删掉（`MAOpt::eraseObservationsIf`，`src/instrumentation/MAIR/MAOpt.h`）。否则它们会与要查的访问合并，使整个合并 Observation 退回 native；在 7zip/re2/sqlite3 上，退回的多来源 Observation 全部含这样的来源（多数只由它们组成，另有 4/72/20 个拖回了要查的访问）。删除是可靠的：Observation 只会覆盖或合并别的 Observation，删掉它不会去掉其他访问的检查（反而避免了它覆盖一个要查的访问、自己退回 native 不查、把那次检查一起带走）；它的来源不再被建模，交给 native，native 照旧跳过。`isSafeAccess` 只取决于地址和访问大小，与检查放在哪无关。之后 `CanInstrumentViaMAIR` 不必再逐来源问 native。测试：`test/MAIR/TestCases/asan/mair-unchecked-sources.ll`（IR 与 select 统计）、`mair-unchecked-sources-oob.c`（越界仍报告）。

- 合并区间没有空洞：`AbstractAccess::merge` 只合并重叠或相邻的区间，`mergeExpressionSizedRanges` 要求区间长度不小于起点差，LIOM/COM/LPOC 只合并相等的区域，periodic 区域只在每轮足迹相接或重叠时把 size 放大，交错合并不放大 size 只缩小 stride（见“地址合并”）。所以查整个区间不会查到来源没碰过的字节。
- 区间起点的对齐由 MAIR 在建图和合并时维护，ASan 直接读取（`AbstractAccess::getAlign`，见 `docs/mair-region-alignment.md`）。例如 `struct { long a; int b, c; }` 的三个字段合并后，起点仍按 8 字节对齐。
- 单个未合并的访问（含固定宽度向量）照旧按 native 方式检查。不超过 16 字节的区间走内联检查：对齐时一次 shadow load，否则查首末两个字节，与 native 对不对齐访问的做法相同；中间至多跨一个 granule，ASan 自己的 redzone 不会这么窄。更长的区间或长度是表达式的区间调用 `__asan_loadN/storeN`，逐字节检查（`__asan_region_is_poisoned`）。
- periodic 区域调用 `__asan_{load,store}_periodic(start, stride, size, count)`（recover 模式带 `_noabort`，`src/runtime/lib/asan/asan_rtl.cpp`）。runtime 按 stride 符号算出最低地址，count 为 0 不查；先用一次 shadow 扫描查整个外包区间，干净则全部合法；否则（或 stride 超过 1 KiB 时）按迭代顺序逐个访问检查，报告第一个越界访问的地址和单次访问大小，空隙中的 poison 不报。内核 ASan 和 `-asan-force-experiment` 没有这个入口，periodic 区域退回 native。
- 报告：读写种类（READ/WRITE，periodic 也一样选 `__asan_load_periodic` 或 `__asan_store_periodic`）和调试位置都取最先执行的来源，与 native 先查到的那次访问一致；大小可能是合并后的区间大小。来源的先后按指令支配关系判断（同一块内即指令顺序），在物化前用 MAIR 图的 DT 算好（`getFirstSource`，`src/instrumentation/llvm23/AddressSanitizer.cpp`），因为物化和先发出的检查会改 CFG 而不更新 DT。来源列表的顺序是合并顺序（LocationMerge 按偏移排序），不是执行顺序，不能直接取第一个。来自不同路径（如分支两侧经 COM 合并）的来源可能没有一个支配其余全部；这时取一个不被其他来源严格支配的来源，只由来源列表顺序决定，结果确定，但不代表运行时实际先执行的那个。`x[i]++` 因此按读报告，`p[0] = p[1]` 合并后的区间也按读报告，位置在 load。测试：`test/MAIR/TestCases/asan/mair-report-first-source.c`。
- 检查放在 Observation 所在的 ProgramPoint：LPOC 的区间/periodic 观察在循环出口（嵌套循环可再被外层不变外提提到外层 preheader），即访问发生之后，这是 ASan 已有的规则（见上文“输入与优化规则”）。

测试：`test/MAIR/TestCases/asan/mair-merged-range.c`、`mair-loop-range.c`（IR），`mair-range-runtime.c`（越界仍报告、界内不误报，含只有空隙被 poison 的 strided 访问），`mair-periodic-runtime.c`（直接调用 runtime 入口的边界情况）。

## TSan 的 lowering

TSan 同样按 Observation 的区域记录访问（`XsanThreadSanitizer::instrumentMAIRObservation` 和 `sanitizeFunctionViaMAIR` 里的 `CanInstrumentViaMAIR`，`src/instrumentation/llvm23/ThreadSanitizer.cpp`）。native TSan 永远不记录的 load/store（委托给 XSan、swifterror、`shouldInstrumentReadWriteFromAddress` 为假、读常量数据、未被捕获的 alloca）与 ASan 一样在优化前删掉（`NativeTsanOmits`，测试 `test/MAIR/TestCases/tsan/mair-unrecorded-sources.ll`）。vtable 指针访问也在优化前删掉，交回 native 的 `__tsan_vptr_{read,update}`：它们给访问打上 vptr 标记，竞态才报成 "data race on vptr"，range 与普通入口都表达不了这一点，所以 vptr 访问不参与 MAIR 优化，既不覆盖也不合并普通访问。剩下的每个来源还要是 load/store 且不是 TSan atomic。固定长度向量与 native 一样按整个存储大小算一次访问，可以与标量访问合并；scalable 向量的大小不是常量，区域检查拒绝它（native 也不记录）。测试 `test/MAIR/TestCases/tsan/mair-vector-vptr.ll`、`mair-race-vptr.cpp`。开 `-ts-distinguish-volatile` 时含 volatile 来源的 Observation 退回 native。TSan 的读、写、读写层次互不相同，合并只发生在同层次之间，所以不会把读记成写。

- 连续区间：2 的幂且不超过 16 字节时，与 native 单次访问相同地选择 `__tsan_{read,write}N`、`unaligned` 或 compound 变体（起点对齐取自 MAIR 区域，见 `docs/mair-region-alignment.md`）；其他大小（如 12、32 字节的向量，native 不记录）或运行时大小用 `__tsan_{read,write}_range`。LLVM 的 load/store 访问的是类型的 store size，总是整字节（i129 存 17 字节），所以不存在“非整字节”的访问。
- periodic 区域：常量 stride 不大于 access size 时足迹连续，按 `[Start, Start + (Trips - 1) * |Stride| + AccessSize)` 记一次 range（下降 stride 起点下移，trip 为 0 时 size 为 0，runtime 直接返回）。其他情况（有空隙或 stride 只在运行时已知）调用 `__tsan_{read,write}_periodic(start, stride, size, count)`（`src/runtime/lib/tsan/rtl/tsan_interface.inc`），runtime 逐轮记录 `size` 字节，空隙不记。
- 报告位置取最先执行的来源（`mair::getFirstSource`，`src/instrumentation/MAIR/Materialization/MAIRMaterialization.cpp`，与 ASan 共用）；访问类型就是 Observation 的类型。循环区间在出口处记录，所以与之竞争的另一线程访问常表现为 “Write … Previous read”。

测试：`test/MAIR/TestCases/tsan/mair-merged-access.c`、`mair-loop-range.c`、`mair-strided-loop.c`、`mair-report-first-source.c`（IR），`mair-race-*.c`、`mair-strided-loop.c`（竞争仍报告、空隙与界外不误报）。

## 可复现验证

- `test/MAIR/TestCases/loop-hierarchy.ll`：循环前/循环内检查隔离、循环内冗余、嵌套图、多个出口及无出口。
- `test/MAIR/TestCases/loop-normalization.ll`：缺少 preheader 和共享出口的输入规范化。
- `test/MAIR/TestCases/loop-irreducible.ll`：不可归约环不中断构图、不建循环层；环内地址不变的访问照常删除，地址随环变化的访问不被绕环的后支配者覆盖。
- `test/MAIR/TestCases/loop-invariant-observation-motion.ll` 与 `loop-range-aggregation.ll`：合法循环专用导出及重复执行稳定性。`repeated_row32` 同时检查内层周期合并得到的 128 字节读，以及随后不变外提把它提到外层 preheader。
- `test/MAIR/TestCases/mair-stats.ll`：`-mair-stats` 把第二次不变外提和内层区间合并分开打印。
- `src/instrumentation/MAIR/unittests`：图边界、方向 barrier、整树生命周期和 guard 物化回归。其中 `KeepsAccessCoveredOnlyInALaterIteration`、`EliminatesInvariantAccessCoveredInALaterIteration`、`IrreducibleCyclesAreOptimizedWithinOneIteration`、`CountsBarriersOfAnEndpointThatRunsAgain`、`DoesNotHoistAnAccessTheLoopCanExitWithout` 对应上面几条规则。
- `src/instrumentation/MAIR/unittests/RandomizedSoundnessTest.cpp`：随机生成带嵌套循环和不可归约环的函数，随机执行，要求每次写都有同地址、中间无 barrier 的 Observation，且每个 Observation 都有这样的写。
- `test/MAIR/TestCases/asan/mair-nonterminating-loop.ll`：终止假设下，ASan 的检查只留在后支配者处。
- `test/MAIR/TestCases/asan/mair-may-not-return.c`：ASan 的检查不会落到可能不返回的调用之后。`mair-nonterminating-loop.ll` 同时说明 PostDom 覆盖对 ASan 生效。
- `test/MAIR/TestCases/asan/mair-atomic.cpp`：两个互相覆盖的原子读只留第一个的检查。
