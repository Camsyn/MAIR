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

构图前使用 LLVM `LoopSimplifyPass` 建立真实 preheader 和 dedicated exits，再取得构图依赖的分析。构图本身不修改 LLVM IR。直接调用构图 API 的调用者也必须满足这些前提。不能形成这些边界时直接报告错误，不创建假边界。不可归约或不能由 LoopInfo 表示的环当前不支持，构图会明确拒绝。

每个优化阶段按子图优先顺序执行，随后处理父图。普通冗余删除、位置合并和公共 Observation 移动继续使用本层 MAIR。它们不能沿父子图链接复用检查。循环不变量移动和周期合并可以显式导出到父图中的循环边界，嵌套导出每次只跨一层，逐层重新证明。内层周期合并会把新的区间观察放到父图的循环出口，而父层第一次不变外提此时已经结束。`default<Og>` 和 `default<O1>` 都在周期合并后再跑一次不变外提，让外层循环重新证明这条观察。

优化输入统计按每次本层 pass 调用累计；Observation 导出后再次作为父层输入，会再次计入该统计，这不表示重复物化。`print<stat>` 则汇总当前整树的 ProgramPoint 和 Observation 数量。

宿主 LLVM 的 `-stats` 在 Release 构建里不会打印。要看每个 pass 实际改了什么，用 `-mair-stats`。`xclang -mair-stats` 会把它转成 `-mllvm -mair-stats`；`mair-opt -mair-stats` 直接认这个选项。每个编译单元在 stderr 打一块汇总，不按函数展开，避免 C++ mangled 名字淹没构建日志。`default<O1>` / `default<Og>` 里重复出现的 pass 带 `#1`、`#2`，所以第二次不变外提不会和第一次混在一起。`in` 仍按每次本层调用累计；比较效果看 `preheader`、`exit`、`range`、`periodic`、`merged`、`intra`、`inter`、`hoisted`、`sunk`。

循环 barrier 摘要来自构图时的 LLVM 指令，并包含子循环。导出 Observation 不会抹掉摘要。当前最小规则是：提升、下沉和周期合并都要求循环中正向、反向均无 barrier。任一迭代中的 barrier 都阻止相应导出，不需要逐动态回边路径查询。这是保守条件，并未放宽既有必执行、SSA 可用性、精确计数、affine recurrence 或 guard 条件。ASan 仍禁止搬动 Observation。

带“来源访问执行过”条件的下沉，以本次循环调用的 preheader 为 guard 重置位置。嵌套循环的下一次调用不会继承上一次调用的 true 状态。整树统一生成物化计划，统一 prepare/apply；完成后整树都进入 consumed 状态。

## 可复现验证

- `test/MAIR/TestCases/loop-hierarchy.ll`：循环前/循环内检查隔离、循环内冗余、嵌套图、多个出口及无出口。
- `test/MAIR/TestCases/loop-normalization.ll`：缺少 preheader 和共享出口的输入规范化。
- `test/MAIR/TestCases/loop-irreducible.ll`：不可归约环明确报错。
- `test/MAIR/TestCases/loop-invariant-observation-motion.ll` 与 `loop-range-aggregation.ll`：合法循环专用导出及重复执行稳定性。`repeated_row32` 同时检查内层周期合并得到的 128 字节读，以及随后不变外提把它提到外层 preheader。
- `test/MAIR/TestCases/mair-stats.ll`：`-mair-stats` 把第二次不变外提和内层区间合并分开打印。
- `src/instrumentation/MAIR/unittests`：图边界、方向 barrier、整树生命周期和 guard 物化回归。

在 `build` 中运行 `ninja check-mair` 执行 MAIR 回归。
