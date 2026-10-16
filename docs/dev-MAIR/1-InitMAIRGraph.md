# InitMAIRGraph

本文保留基础 IR 的初始设计记录，术语已同步为 Program Point（PP）和 Observation（o）；阶段目标与验收项不等于当前完整接口规范。当前结构与行为以 `src/instrumentation/MAIR/MAIR.h`、`MAIR.cpp`、`unittests/MAIRGraphTest.cpp` 及 `test/MAIR/TestCases/` 的已验证实现为准。`SPEC.md` 作为语义意图与正确性义务参考，存在差异时应先核对实现与测试。

## 背景与动机

当前项目的主线是把 XSan 的 memory access 优化能力逐步迁移到 MAIR 上。MAIR 的目标不是复刻 LLVM IR，而是抽出一层更稳定的高阶 IR，用来表达 Program Point（PP）、Observation（o）及其组织关系。

这一步只做基础数据结构和工具函数的设计与落地，不接入 XSan 的正式优化链路，不实现最终的 sanitizer lowering。

## 本阶段目标

实现一套可用的 MAIR 基础 IR 层，覆盖以下对象：

- `MAIRGraph`
- `ProgramPoint`
- `Observation`

要求它们在设计上显式对齐 LLVM 的 `CFG / BasicBlock / Instruction` 三级结构，便于后续复用 LLVM 的遍历、插入、迭代、dump 和测试习惯。这里的三级结构是本阶段最重要的设计锚点，不能被弱化为普通容器关系。

对应关系必须在实现和文档中清晰成立：

| MAIR 层级 | 对齐的 LLVM 层级 | 设计含义 |
| --- | --- | --- |
| `MAIRGraph` | `CFG` | function-local 的结构层，负责 program point (PP) 之间的边、遍历、reachability 和整体校验。 |
| `ProgramPoint` | `BasicBlock` | graph 内的局部组织单元，承载附着在同一 program point (PP) 上的一组 `Observation`，并提供类似 BB 的局部迭代与修改入口。 |
| `Observation` | `Instruction` | 最小 payload / operation 单元，表示一次被观测的 memory location 与 access type，并关联原始 LLVM instruction / value。 |

这里的“对齐”指包含关系和接口习惯：`MAIRGraph -> ProgramPoint (PP) -> Observation (o)` 类比 `CFG -> BasicBlock -> Instruction`，不表示语义一一等同。一个 LLVM basic block 可以对应多个 PP；PP 的 Observation 列表顺序不代表原程序执行次序。图边记录带类型的 Dom / PDom 关系，不是 CFG successor 边。

因此，验收时必须能从代码和测试中看出以下结构关系：

- 遍历 `MAIRGraph` 得到 `ProgramPoint`，类似遍历 function / CFG 得到 basic block。
- 遍历 `ProgramPoint` 得到 `Observation`，类似遍历 `BasicBlock` 得到 `Instruction`。
- `Observation` 保持对 LLVM 原始 `Instruction` / `Value` 的轻量引用，不复制 LLVM IR 语义。
- pure dump 以 `MAIRGraph -> ProgramPoint -> Observation` 的嵌套层次输出；annotated dump 以 LLVM IR 为主，在对应 `BasicBlock` / `Instruction` 附近显示 MAIR 注释。

## 设计原则

1. 性能优先，结构要接近 LLVM 的局部对象模型，不要引入不必要的全局索引和重型抽象。
2. 尽量复用 LLVM 现有接口与惯用法，优先使用 `Value` / `Instruction` / `BasicBlock` / iterator 体系能直接支持的能力。
3. MAIR 先作为独立 IR 层存在，再考虑和 XSan 连接。
4. 设计必须服务于后续优化、分析和测试，不预埋无依据的 fallback。
5. 当前工作不考虑 LLVM15 适配，仅面向现有项目实际支持的 LLVM 版本演进。
6. 参考实现必须明确对照 LLVM 源码路径，不能只写抽象类名不写出处；文档中同时保留 LLVM 根路径和相对路径，方便人工核查。
7. 尽可能复用 LLVM 暴露出来的接口。

## 参考语义

初始设计参考 `SPEC.md` 的 MAIR 意图，尤其是：

- Observation（o）的核心是 `memory location + access type`
- graph layer 只是 derived representation，不替代 core MAIR relation
- program point (PP) 可以来自 memory-access instruction，也可以来自 basic block entry
- 后续优化会依赖 graph reachability、barrier、location / access type 的组合语义

### 初始概念到实现对象的映射

| SPEC 概念 | 本阶段实现对象 | 约束 |
| --- | --- | --- |
| instruction-level / basic-block boundary PP | `ProgramPoint` 的 site kind | 一个 `ProgramPoint` 对应一个 PP；当前 site 还区分 basic-block exit 与 virtual edge，详见 `ProgramPointSite::Kind`。 |
| `core(o) = (l, t) \in L \times T` | `Observation` 的核心描述 | 一个 `Observation` 表示一个 memory location 与 access type 的组合，不承载 graph edge 职责。 |
| `payload(pp)` | `ProgramPoint` 内的 `Observation` 列表 | 同一 program point (PP) 上可以挂一个或多个 `Observation`，列表顺序应稳定可遍历。 |
| derived MAIR graph | `MAIRGraph` | graph 是 function-local 的辅助表示，管理 `ProgramPoint` 节点与边，不替代 core MAIR relation。 |

## LLVM 参考路径

本阶段设计和实现时，必须参考本机 LLVM 项目：

- LLVM 项目根路径：`/home/camsyn/forks/llvm-project/llvm`

必须显式参考以下源码路径，文档、代码注释或 worklog 中不能只写抽象类名而不写出处：

| 绝对路径 | 相对 LLVM 根路径 | 本阶段参考重点 |
| --- | --- | --- |
| `/home/camsyn/forks/llvm-project/llvm/include/llvm/IR/CFG.h` | `include/llvm/IR/CFG.h` | `successors` / `predecessors` / CFG 遍历和 graph view 习惯。 |
| `/home/camsyn/forks/llvm-project/llvm/include/llvm/IR/Function.h` | `include/llvm/IR/Function.h` | function-local block 容器、顶层 ownership、迭代入口。 |
| `/home/camsyn/forks/llvm-project/llvm/include/llvm/IR/BasicBlock.h` | `include/llvm/IR/BasicBlock.h` | basic block 内 instruction list、插入/删除/遍历、parent linkage、局部 dump。 |
| `/home/camsyn/forks/llvm-project/llvm/include/llvm/IR/Instruction.h` | `include/llvm/IR/Instruction.h` | instruction 作为 leaf operation 的 parent/query/source location 风格。 |
| `/home/camsyn/forks/llvm-project/llvm/include/llvm/IR/AssemblyAnnotationWriter.h` | `include/llvm/IR/AssemblyAnnotationWriter.h` | annotated dump 如何附着到 LLVM IR 输出。 |

参考重点不是复制代码，而是借鉴它们的分层方式、对象生命周期、ilist / iterator 风格、查询接口和 dump 习惯。

最低要求：

- `MAIRGraph` 的 public API 应让使用者自然联想到 LLVM CFG / function-local graph 的遍历和校验方式。
- `ProgramPoint` 的 public API 应让使用者自然联想到 `BasicBlock` 对 instruction list 的组织方式。
- `Observation` 的 public API 应让使用者自然联想到 `Instruction` 作为最小可遍历、可打印、可定位操作单元的使用方式。
- annotated dump 必须参考 `AssemblyAnnotationWriter` 的输出模式，避免自造一套与 LLVM IR dump 割裂的格式。

## 需要设计的数据结构

### 1. `Observation`

表示一个有独立身份的内存观测项（Observation，简称 o），描述要观测的区域、读写类型、属性与来源。

结构定位：`Observation` 对齐 LLVM `Instruction`。它是 `ProgramPoint` 内部的最小元素，应该支持在 `ProgramPoint` 中稳定遍历、打印、定位和后续 rewrite。

最低要求：

- 能表达 read / write / read-write 语义
- 能表达访问大小或等价的布局信息
- 能关联到 LLVM 中的原始内存访问来源
- 能复用 LLVM 的 `Instruction` / `Value` / `MemoryLocation` / `DataLayout` 相关能力

### 2. `ProgramPoint`

表示一个 Program Point（简称 PP）及其承载的 Observation（简称 o）。

结构定位：`ProgramPoint` 对齐 LLVM `BasicBlock`。它不是普通记录，而是 `MAIRGraph` 中的局部组织单元，负责保存一组有顺序的 `Observation`，并暴露类似 basic block 的局部迭代和修改接口。

最低要求：

- 能表达 program point (PP) 的来源
- 能承载一个或多个 `Observation`
- 能区分 instruction-level PP 与 basic-block-entry PP
- 能为后续 graph rewrite 保留必要的 proof/context 信息

### 3. `MAIRGraph`

表示一个 function-local 的 MAIR 有向图。

结构定位：`MAIRGraph` 对齐 LLVM CFG。它管理 function 内所有 `ProgramPoint` 及其带类型关系边，负责整体生命周期、遍历入口、结构校验和 dump。

最低要求：

- 节点语义要贴近 LLVM 的 basic block / instruction 组织方式
- 节点与 payload 分离，graph 负责结构，`ProgramPoint` 负责内容
- 能表达节点之间的 reachability / edge 关系
- 能高效遍历、插入、删除、合并节点
- 能支持后续导出到 dump 和测试断言

## 接口要求

实现时应优先提供以下类别的方法：

- 构造与查询
- 图节点（PP）/ Observation 迭代
- 节点插入、删除、替换、合并
- 基于 LLVM 风格的 `dump` / `print` 接口
- 基础校验接口，例如 consistency / well-formed 检查

接口命名应尽量贴近 LLVM 习惯，避免引入与 LLVM 风格冲突的抽象词。

### 最小 API / 行为清单

本阶段不要求复刻 LLVM `Function` / `BasicBlock` / `Instruction` 的全部接口，但至少要有以下可测试行为：

- `MAIRGraph`: function 关联、`ProgramPoint` 创建/插入/移除、graph-level begin/end 或等价迭代、successor/predecessor 查询、edge 添加/移除、`verify`、`print` / `dump`。
- `ProgramPoint`: point kind 查询、所属 `MAIRGraph` 查询、关联 `BasicBlock` / anchor `Instruction` 查询、`Observation` 创建/插入/移除、begin/end 或等价迭代、`print` / `dump`。
- `Observation`: access type 查询、location/source 查询、所属 `ProgramPoint` 查询、访问大小或布局信息查询、`print` / `dump`。
- 修改操作要保持 parent linkage 和遍历稳定性；不满足不变量时应 fail-fast，不做隐藏 fallback。

## Dump 形式

必须提供两种 dump：

### 1. Pure dump

MAIR 作为独立 IR 输出，风格接近 LLVM IR。

要求：

- 只展示 MAIR 自身结构
- 适合作为调试、单测 golden file、regr test 的主要输出
- 稳定、可读、尽量不依赖 LLVM 原始 IR 的格式噪音
- 必须体现 `MAIRGraph -> ProgramPoint -> Observation` 的三层嵌套关系

### 2. Annotated dump

MAIR 作为 LLVM IR 的注释输出，风格类比 LLVM 的 MSSA dump。

要求：

- 以原始 LLVM IR 为主
- 在相关位置附加 MAIR 注释
- 能看出 PP / Observation 附着在何处
- 适合作为和 LLVM IR 对照的调试输出
- 必须能区分注释附着在 basic-block-entry PP 还是 instruction-level PP

## 测试要求

测试必须遵循两层：

1. `google unittest`
2. `lit` e2e regression test

最低要求：

- 为 `MAIRGraph / ProgramPoint / Observation` 提供单元测试
- 本阶段单元测试至少覆盖 instruction-level PP 与 basic-block-entry PP；当前完整 site 种类见 `ProgramPointSite::Kind`
- 单元测试必须验证三层遍历链条: `MAIRGraph -> ProgramPoint -> Observation`
- 单元测试必须覆盖 well-formed 检查，至少包括 parent linkage、point kind、payload 归属、空 PP、多 Observation 的 PP
- 为 pure dump 提供稳定的文本回归测试
- 为 annotated dump 提供 e2e 回归测试
- annotated dump 回归测试必须同时覆盖 basic-block-entry annotation 和 instruction-level annotation
- 新增测试后，相关测试必须全部通过

## 本阶段不做的事

- 不接入 XSan 正式优化链路
- 不替换现有 hardcoded 优化
- 不要求完成 LLVM15 适配
- 不要求实现完整 sanitizer lowering
- 不要求把 MAIR 直接接到 runtime
- 不要求复刻 LLVM `BasicBlock` / `Instruction` 全接口，只要求在容器分层、遍历、局部修改、dump / verify 风格上同构

## 验收标准

当且仅当满足以下条件时，本阶段完成：

1. 仓库中已新增 MAIR 基础 IR 设计实现，包含 `MAIRGraph`、`ProgramPoint`、`Observation`。
2. 设计文档、代码结构和测试必须显式体现 `MAIRGraph / ProgramPoint / Observation` 对齐 `CFG / BasicBlock / Instruction` 的三级关系。
3. 设计文档和 worklog 必须写明 LLVM 参考项目根路径 `/home/camsyn/forks/llvm-project/llvm`，并至少覆盖 `CFG.h`、`BasicBlock.h`、`Instruction.h`、`AssemblyAnnotationWriter.h` 的参考理由。
4. 这些对象能在 LLVM 风格的接口下完成构造、查询、遍历、修改和校验。
5. `ProgramPoint` 必须明确表达一个 program point (PP)，并区分 instruction-level 与 basic-block-boundary site；`Observation` 的核心描述为 `core(o) = (l, t)`，且不承担 graph edge 职责。
6. `verify` 或等价 well-formed 检查必须覆盖 parent linkage、point kind、payload 归属、空 PP、多 Observation 的 PP。
7. 同时支持 pure dump 和 annotated dump；pure dump 展示三层嵌套，annotated dump 区分 BB-entry 与 instruction-level 注释。
8. 已新增对应的 `google unittest`，覆盖三层遍历、两类 program point (PP)、修改操作和 well-formed 检查。
9. 已新增对应的 `lit` e2e regression test，覆盖 pure dump 与 annotated dump。
10. 相关测试在本地和 CI 目标上均保持全绿。
11. 任务过程有持续留痕，关键决策记录在 `.tasks/init_mairgraph_20260629_2330/worklog.md`。

## 实施顺序建议

1. 先补最小可用的数据结构草案。
2. 再补 pure dump。
3. 再补 annotated dump。
4. 然后补单元测试。
5. 最后补 lit e2e 测试并收敛接口。

## 备注

这份文档只定义本阶段交付物和验收条件，不定义 MAIR 全部后续优化策略。后续把现有 hardcoded 优化迁移到 MAIR 上时，应另起文档分阶段推进。
