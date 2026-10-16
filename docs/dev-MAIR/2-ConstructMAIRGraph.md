# 2. 从 `llvm::Function` 构建 `MAIRGraph`

本文保留图构建的初始设计记录，术语已同步为 Program Point（PP）和 Observation（o）。后文的阶段目标和拆分建议不是当前完整接口规范；当前实现以 `src/instrumentation/MAIR/MAIR.h`、`MAIR.cpp`、`MAIRBarrierAnalysis.h` 和对应测试为准。当前公开构建入口是 `MAIRGraph::build`，没有 `CreateFrom` / `createFrom` API。

## 问题与动机

`ActiveMopAnalysis` 现在解决的是“给定两个 memory operation，判断可达性”的问题；它是查询型分析，不是可复用的 IR 结构。

MAIR 这一层要解决的是另一件事：把一个 `llvm::Function` 先构造成稳定、可遍历、可 dump 的高阶 IR 图，然后后续优化只在图上做，不再重复做 sanitizer 可达性分析。

这一步的核心价值有三个：

1. 把“程序点（PP）/观测（o）/边”的语义显式化；
2. 把 sanitizer-specific 逻辑封装到可替换策略里；
3. 为后续替换 `src/instrumentation/Analysis` 和 `src/instrumentation/Instrumentation.cpp` 里的 hardcoded 优化做前置铺垫。

## 参考范围

- 语义意图与正确性义务参考：`/home/camsyn/projects/MAIR/SPEC.md`
- 当前参考实现：`/home/camsyn/projects/MAIR/src/instrumentation/Analysis/ActiveMopAnalysis.h`
- 当前参考实现：`/home/camsyn/projects/MAIR/src/instrumentation/Analysis/ActiveMopAnalysis.cpp`
- 现有 MAIR 代码：`/home/camsyn/projects/MAIR/src/instrumentation/MAIR/MAIR.h`
- 现有 MAIR 代码：`/home/camsyn/projects/MAIR/src/instrumentation/MAIR/MAIR.cpp`
- MAIR 单测：`/home/camsyn/projects/MAIR/src/instrumentation/MAIR/unittests/MAIRGraphTest.cpp`
- MAIR lit：`/home/camsyn/projects/MAIR/test/MAIR/TestCases/llvm-dump.ll`
- LLVM 参考项目路径：`/home/camsyn/forks/llvm-project/llvm`
- LLVM 参考头文件：`/home/camsyn/forks/llvm-project/llvm/include/llvm/IR/BasicBlock.h`

## 目标

通过 `MAIRGraph::build(Function &F, ...)`，把函数内的 Program Point（PP）和 Observation（o）构造成原始 `MAIRGraph`。

这里的“原始”含义是：

- 图一旦构建完成，就不再依赖 sanitizer 可达性查询来回答 graph 内部的连通性；
- graph 边记录经过相应方向 barrier 检查的 Dom / PDom 候选关系，不单独授权删除或移动 Observation；
- 后续优化只消费这个图，不再回头跑 `ActiveMopAnalysis` 这类查询式分析。

## 对齐关系

`MAIRGraph / ProgramPoint / Observation` 要显式对齐 LLVM 的三层结构：

| LLVM 结构 | MAIR 结构 | 责任 |
| --- | --- | --- |
| `CFG` | `MAIRGraph` | 承载函数内的局部图与边 |
| `BasicBlock` | `ProgramPoint` | 承载一个 PP，可以位于 block 边界、memory instruction site 或控制流边 |
| `Instruction` | `Observation` | 承载一次有独立身份的观测及其 location/kind |

这里是包含关系和接口习惯的类比，不是一一映射。一个 basic block 可产生多个 PP，PP 中 Observation 的排列不表示原指令执行次序；图边记录经过 barrier 过滤的 Dom / PDom 关系，不是 CFG successor 边。

这一步不要把三层揉成一层，也不要把 edge 构建逻辑塞进 dump 逻辑里。

## 设计决策

### 1. 构建入口

当前函数级公开构建入口为 `MAIRGraph::build<BarrierTrait>(...)`。

FAM 入口借用 LLVM New Pass Manager 已维护的分析结果：

- `llvm::Function &F`
- `llvm::FunctionAnalysisManager &FAM`
- 可选 `MAIROptimizationConfig`

该入口通过 `FAM.getResult<DominatorTreeAnalysis>(F)`、
`FAM.getResult<PostDominatorTreeAnalysis>(F)` 和 `FAM.getResult<LoopAnalysis>(F)` 获取 DT/PDT/LI。图存活期间这些结果须保持有效，底层 LLVM IR 不得改变。

另一个公开入口接收右值分析并将所有权移交给图：

- `llvm::Function &F`
- `llvm::DominatorTree &&DT`
- `llvm::PostDominatorTree &&PDT`
- `llvm::LoopInfo &&LI`
- 可选 `MAIROptimizationConfig`

两个入口均通过模板参数选择 barrier trait，并调用同一个内部 `build` 实现。保留分析所有权的调用者应使用 FAM 入口。

`DataLayout` 只用于 `Observation::fromInstruction` 一类的 memory location 提取，不应污染 edge 逻辑。

`MAIR` 核心实现不应在内部 `build` 中构造 `DominatorTree DT(F)` /
`PostDominatorTree PDT(F)`，也不得手写替代性的 dominance /
post-dominance 求解器。分析结果的生命周期和缓存由 caller 或 FAM 负责。

### 2. 策略抽象（初始建议）

把 sanitizer barrier 逻辑做成模板 + trait，而不是硬编码进 `MAIRGraph`。

建议形态：

- `template <class BarrierTrait> class MAIRGraph`
  或
- `template <class BarrierTrait> class MAIRGraphConstructor`

trait 至少提供：

- forward barrier 判断
- backward barrier 判断
- 可选的 instruction 过滤接口

这样做的原因是：

- sanitizer 差异只体现在 barrier 传播上；
- graph 数据结构本身应尽量稳定；
- 后续替换 ASan / TSan / 其他策略时，不应重写图构建器主体。

### 3. 架构拆分（初始建议）

初始设计建议在构建逻辑复杂后拆分构造器/分析器，避免把所有逻辑堆在一个函数里。

建议拆成两层：

- `MAIRGraphConstructor`：负责枚举 PP、收集 Observation、落图；
- `MAIRBarrierAnalysis`：负责基于 caller/FAM 提供的 `DT/PDT + BarrierTrait` 判断 edge 是否成立。

这样可以把“收集”和“连边”分开，便于测试和维护。当前实现没有 `MAIRGraphConstructor` 类；落图逻辑在 `MAIRGraph::build`，barrier 查询由 `MAIRBarrierAnalysis` 提供。

### 4. 边定义

以下公式记录初始设计中基于 idom / ipdom 与 barrier 的候选关系动机，不作为当前图边的完整定义：

```text
edge_s(pp1, pp2)
iff
(pp1 idom pp2 and pp1 leadsto_fwd_s pp2)
or
(pp1 ipdom pp2 and pp1 leadsto_bwd_s pp2)
```

初始公式强调支配候选与 barrier 检查共同参与构建。核对当前实现时，应区分：

- 跨块候选来自 `idom / ipdom` 边界，块内候选来自相邻 PP；
- 相应方向的 barrier 检查是成边条件，不是事后注释；
- 当前实现对块内相邻 PP 和跨块 IDom / IPDom 边界分别构建候选边，再按方向检查 barrier；优化仍需检查区域、类型、guard、循环与 policy。

### 5. 性能约束

构建过程要尽量接近 LLVM 风格：

- 只做函数内局部遍历；
- 尽量复用 LLVM 自带的 `Instruction`、`BasicBlock`、`DominatorTree`、`PostDominatorTree`、`DataLayout`；
- `DominatorTree` / `PostDominatorTree` 必须来自 FAM 或 caller 注入，不在 MAIR 核心中重复计算；
- 不复制不必要的 IR 对象；
- barrier reachability 不能做全量暴力枚举，必须有按 block 或按区间的缓存/摘要。

### 6. 可读性与可维护性

- PP 枚举、Observation 提取、barrier 摘要、edge 判定要分层；
- 函数体内逻辑要短，命名要能直接对应 `SPEC.md`；
- 注释要解释“为什么这样做”，不要重复 API 名字；
- 不引入 LLVM15 兼容分支，这一轮只对齐当前 LLVM23 代码基线。

## 需要实现的构建流程

1. 遍历 `Function`，识别候选 Program Point。
2. 为每个 basic block 建立入口与出口 `ProgramPoint`，为每条被建模的 memory instruction 建立独立 `ProgramPoint`。
3. 提取 memory instruction 的 `Observation`，并挂到对应 `ProgramPoint` 上；例如 `memcpy` 的 PP 同时包含源读与目标写。
4. 计算 `DT/PDT` 约束下的候选边。
5. 用 barrier trait 过滤掉不满足传播条件的边。
6. 产出最终 `MAIRGraph`。

## 验收标准

### 功能验收

- 能从一个 `llvm::Function` 构建出完整 `MAIRGraph`。
- 构建结果能同时支持 pure dump 和 annotated dump。
- 构建结果符合 `MAIRGraph::build` 的已验证边语义；`SPEC.md` 为语义意图与正确性义务参考。
- 构建结果不依赖后续 sanitizer reachability 查询。

### 接口验收

- FAM 入口为 `build<BarrierTrait>(Function&, FunctionAnalysisManager&, Config)`，借用 DT/PDT/LI。
- 所有权入口为 `build<BarrierTrait>(Function&, DominatorTree&&, PostDominatorTree&&, LoopInfo&&, Config)`，由图接管分析生命周期。
- `src/instrumentation/MAIR/MAIR.cpp` 中不得出现 `DominatorTree DT(F)`、`PostDominatorTree PDT(F)`、`computePostDominators(...)` 这类内部重建分析的代码。
- 同一函数通过 FAM 入口和移交 DT/PDT/LI 所有权的入口构建出的 graph 必须一致。
- barrier 逻辑可通过 trait 替换，不需要改 graph 数据结构本体。
- `ProgramPoint / Observation` 的职责边界保持清晰。

### 测试验收

- 新增 `gtest` 单测覆盖：
  - 线性 CFG
  - 分支 CFG
  - `idom` 正向边
  - `ipdom` 反向边
  - barrier 命中时断边
  - barrier 不命中时成边
  - 空函数 / 无 memory access 函数
- 新增 `lit` 回归测试覆盖：
  - pure dump
  - annotated dump
  - 构建结果的稳定输出
- `MAIRGraphTest` 全过。
- `check-mair` 全过。

### 质量验收

- 不引入与当前任务无关的 fallback / workaround。
- 不修改 LLVM15 适配逻辑。
- 代码注释足够解释 edge 判定和抽象拆分。

## 实施顺序

1. 先补单测，先写失败用例，再写实现。
2. 先做 `MAIRGraphConstructor` / `BarrierTrait` 的骨架。
3. 先实现最小闭环：单函数、单 block、单边。
4. 再补 `DT/PDT` 约束和 barrier 过滤。
5. 最后把 dump 与 lit 回归补齐。

## 风险与控制

### 风险 1：边判定过早复杂化

控制方式：先做最小可行实现，以当前构建代码和回归测试明确 edge 语义，再扩展已验证的特殊情况。

### 风险 2：barrier 逻辑和图结构耦合过深

控制方式：barrier 逻辑只存在于 trait / analysis 层，`MAIRGraph` 只保留结构和遍历能力。

### 风险 3：构建过程重复扫描 CFG，性能不稳定

控制方式：按 basic block 缓存 barrier 摘要和局部访问摘要，避免重复做同一段路径分析。

## 验证方式

建议按下面顺序验证：

```bash
ninja -C build MAIRGraphTest mair-opt
ninja -C build check-mair
```

重点不是“能跑”，而是：

- 构建结果稳定；
- dump 输出稳定；
- 单测和 lit 都能覆盖边语义；
- 全部测试保持绿色。
