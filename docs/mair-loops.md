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

构图前使用 LLVM `LoopSimplifyPass` 建立真实 preheader 和 dedicated exits，再取得构图依赖的分析。构图本身不修改 LLVM IR。直接调用构图 API 的调用者也必须满足这些前提。不能形成这些边界时直接报告错误，不创建假边界。不可归约或不能由 LoopInfo 表示的环先不优化：环里的 Observation 保持原位。自然循环仍按原规则优化。TODO：以后再表示并优化这种非标准循环。

每个优化阶段按子图优先顺序执行，随后处理父图。普通冗余删除、位置合并和公共 Observation 移动继续使用本层 MAIR。它们不能沿父子图链接复用检查。循环不变量移动和周期合并可以显式导出到父图中的循环边界，嵌套导出每次只跨一层，逐层重新证明。内层周期合并会把新的区间观察放到父图的循环出口，而父层第一次不变外提此时已经结束。`default<Og>` 和 `default<O1>` 都在周期合并后再跑一次不变外提，让外层循环重新证明这条观察。

优化输入统计按每次本层 pass 调用累计；Observation 导出后再次作为父层输入，会再次计入该统计，这不表示重复物化。`print<stat>` 则汇总当前整树的 ProgramPoint 和 Observation 数量。

宿主 LLVM 的 `-stats` 在 Release 构建里不会打印。要看每个 pass 实际改了什么，用 `-mair-stats`。`xclang -mair-stats` 会把它转成 `-mllvm -mair-stats`；`mair-opt -mair-stats` 直接认这个选项。每个编译单元在 stderr 打一块汇总，不按函数展开，避免 C++ mangled 名字淹没构建日志。`default<O1>` / `default<Og>` 里重复出现的 pass 带 `#1`、`#2`，所以第二次不变外提不会和第一次混在一起。`in` 仍按每次本层调用累计；比较效果看 `preheader`、`exit`、`range`、`periodic`、`merged`、`intra`、`inter`、`hoisted`、`sunk`。

统计汇总按函数首次出现的顺序遍历各函数的 pass visits，以 pass 标签首次出现的位置决定行顺序；不是全局事件时间顺序。汇总阶段一次累计，保持 ordinal、字段顺序及只含 `in` 的行，全零行不打印。

循环 barrier 摘要来自构图时的 LLVM 指令，并包含子循环。导出 Observation 不会抹掉摘要。当前最小规则是：提升、下沉和周期合并都要求循环中正向、反向均无 barrier。任一迭代中的 barrier 都阻止相应导出，不需要逐动态回边路径查询。这是保守条件，并未放宽既有必执行、SSA 可用性、精确计数、affine recurrence 或 guard 条件。ASan 允许搬动普通 Observation，检查可以落在实际访问之后。原子访问仍然不能搬。

带“来源访问执行过”条件的下沉，以本次循环调用的 preheader 为 guard 重置位置。嵌套循环的下一次调用不会继承上一次调用的 true 状态。整树统一生成物化计划，统一 prepare/apply；完成后整树都进入 consumed 状态。

内层范围起点复用已有 LLVM Value 时，除 SCEV 相等和目标位置可用外，还必须保证类型匹配与 poison 安全。指令候选只有在 `ScalarEvolution::canReuseInstruction` 允许复用且无需丢弃 poison flags 时才接受；优化阶段不修改原 LLVM IR。`loop-start-reuse.ll` 覆盖不安全候选、非 SCEV 类型以及继续寻找安全候选。

## 地址合并

符号地址只有在能够证明字节差时才合并。有 ScalarEvolution 时使用地址 SCEV 的常量差；证明失败即保留原观察。没有该分析时，只用 LLVM 的常量偏移剥离匹配相同剩余地址，以 APInt 计算差值，不把整数 `add` 当成不回绕的地址距离。

Observation 合并保留完整来源指令序列及重复项。若右项严格包含左项，右项来源放在前面；相等、左项包含或非包含的区间并集仍按左后右追加。首项及首个有效 debug location 依赖这个顺序，不能把来源改成集合。

## 可复现验证

- `test/MAIR/TestCases/loop-hierarchy.ll`：循环前/循环内检查隔离、循环内冗余、嵌套图、多个出口及无出口。
- `test/MAIR/TestCases/loop-normalization.ll`：缺少 preheader 和共享出口的输入规范化。
- `test/MAIR/TestCases/loop-irreducible.ll`：不可归约环不中断构图，环内访问留在原块；外层自然循环仍可合并。
- `test/MAIR/TestCases/loop-invariant-observation-motion.ll` 与 `loop-range-aggregation.ll`：合法循环专用导出及重复执行稳定性。`repeated_row32` 同时检查内层周期合并得到的 128 字节读，以及随后不变外提把它提到外层 preheader。
- `test/MAIR/TestCases/mair-stats.ll`：`-mair-stats` 把第二次不变外提和内层区间合并分开打印。
- `src/instrumentation/MAIR/unittests`：图边界、方向 barrier、整树生命周期和 guard 物化回归。

在 `build` 中运行 `ninja check-mair` 执行 MAIR 回归。
