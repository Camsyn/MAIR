# MAIR：内存访问观测的中间表示

## 1. 问题与范围

Sanitizer 需要决定在程序执行中的哪些位置、以何种访问语义观测哪些内存字节。若直接以源指令或 runtime callback 表达优化，访问的语义、执行条件和观测位置容易被混为一谈。MAIR 将 memory access observation 作为独立的高阶 IR 对象，使这些关系能被显式表述和检验。

本文定义当前实现的语义模型及变换的正确性义务。它不指定某种 LLVM 指令、回调 ABI、图算法或完整的 sanitizer 语义；也不宣称当前实现已经证明了所有义务。

## 2. 静态 MAIR 实例

MAIR 每层的包含关系为 `MAIRGraph -> ProgramPoint (PP) -> Observation (o)`。函数根图唯一拥有自然循环的子图，嵌套循环逐层拥有；每层只包含本层直接内容。PP 承载位置与执行条件，Observation 描述要观测的内存行为。它们类比 LLVM 的 `CFG -> BasicBlock -> Instruction` 包含关系，但不一一映射；一个 basic block 可产生多个 PP，PP 内 Observation 的列表顺序不表示原指令执行次序。

`LoopProgramPoint` 在父图中关联循环 body：每个循环有一个位于真实 preheader `BBExit` 的 `LoopPreheader`，以及零个或多个位于 dedicated exit `BBEntry` 的 `LoopExit`。普通优化边限于同层；只有循环专用变换经过独立证明，才可将 Observation 导出到父层。构图前需由 LLVM LoopSimplify 建立这些真实边界；当前不可归约环明确拒绝。具体生命周期、遍历与输入约束见 [循环结构说明](docs/mair-loops.md)。

令 $L$ 为符号内存区域的集合，$T=\{r,w,rw\}$ 为访问类型集合。一个 Observation $o$ 的核心描述是

$$
\operatorname{core}(o)=(\ell_o,t_o)\in L\times T.
$$

访问类型 $r$、$w$、$rw$ 分别表达读、写、读写。将它们解释为读写能力集合时，类型合并为集合并：

$$
\kappa(r)=\{R\},\quad \kappa(w)=\{W\},\quad
\kappa(rw)=\{R,W\},\qquad
\kappa(t_1\sqcup t_2)=\kappa(t_1)\cup\kappa(t_2).
$$

这个合并运算描述合并后应保留的访问类型，**不**定义任意 sanitizer 的冗余覆盖关系。

对函数 $f$，其有限静态 MAIR 实例定义为

$$
\mathcal M_f=
(\mathsf{PP}_f,O_f,\operatorname{site},\operatorname{guard},
 \operatorname{payload},\operatorname{desc},\operatorname{src}).
$$

其中 $P_f$ 是可用的放置位置集合，$\Gamma_f$ 是执行条件集合，$Q$ 是访问属性集合，$I_f$ 是来源指令集合；各映射满足

$$
\begin{aligned}
\operatorname{site}&:\mathsf{PP}_f\to P_f,&
\operatorname{guard}&:\mathsf{PP}_f\to\Gamma_f,\\
\operatorname{payload}&:\mathsf{PP}_f\to O_f^*,&
\operatorname{desc}&:O_f\to L\times T\times Q,\\
\operatorname{src}&:O_f\to\mathcal P(I_f).
\end{aligned}
$$

具体地：

- $\mathsf{PP}_f$ 是有身份的 Program Point（PP）集合；$\operatorname{site}$ 给出放置位置，$\operatorname{guard}$ 给出该 PP 的执行条件。
- $O_f$ 是有身份的静态 Observation 集合；$\operatorname{payload}(pp)$ 是 PP $pp$ 所承载的 Observation，每个 Observation 在所有 payload 中恰出现一次。
- $\operatorname{desc}(o)=(\ell_o,t_o,q_o)$ 给出区域、访问类型及影响观测的属性；其核心投影为 $\operatorname{core}(o)=(\ell_o,t_o)$。$\operatorname{src}(o)$ 记录该 Observation 代表的来源，而不是其当前放置位置。

静态身份不能用描述值代替：两个 Observation 可以有相同的 $\operatorname{core}(o)$，同一个 site 也可以承载执行条件不同的多个 PP。当前 site 可位于内存指令、基本块入口或出口，以及控制流边；guard 可以是恒真，也可以依赖来源是否曾执行。属性至少保留原访问的原子性、易变性及需要独立处理的调用式访问语义。

### 区域的解释

区域 $\ell\in L$ 可以包含符号值。给定某次动态执行的环境 $\eta$，其含义是被观测的字节集合 $\llbracket\ell\rrbracket_\eta$。当前模型可表达连续区间、由 mask 选择的 lane 集合，以及周期区域；后者的抽象含义是

$$
\llbracket\operatorname{Periodic}(p,d,b,n)\rrbracket_\eta
=\bigcup_{0\leq i<n_\eta}
[p_\eta+i\,d_\eta,\ p_\eta+i\,d_\eta+b_\eta).
$$

这里 $p$ 是完整起始地址；当 $n_\eta=0$ 时区域为空。区域表达、精确比较以及在目标 site 求值是三个不同条件：具有某种符号描述，不意味着可以在任意位置构造该描述，或与另一描述比较、合并。

## 3. 动态语义与正确性目标

令 $\pi$ 是原程序在约定观测作用域内的一次完整、可比较的真实执行，包含线程交错及 sanitizer 所需的其他环境事件。记 $g_{pp}=\operatorname{guard}(pp)$。对每次到达 $\operatorname{site}(pp)$ 的动态实例 $d$，只有在 $g_{pp}(d)$ 成立时，$\operatorname{payload}(pp)$ 才贡献观测事件。对其中的 Observation $o$，动态事件由

$$
\bigl(d,o,\llbracket\ell_o\rrbracket_{\eta_d},t_o,q_o\bigr)
$$

描述。按实际执行及发射次序形成 $\operatorname{Trace}(\mathcal M_f,\pi)$。同一个静态 Observation 可以出现零次或多次；相同的符号表达式在不同循环迭代中也可能指向不同字节。

Sanitizer $s$ 的解释记为

$$
\operatorname{Err}_s(\pi,\mathcal M_f)
=\operatorname{Run}_s\bigl(\pi,\operatorname{Trace}(\mathcal M_f,\pi)\bigr),
$$

其中 $\operatorname{Run}_s$ 可依赖此前状态、线程同步和内存生命周期，不要求每个 MAIR Observation 独立地产生一个固定状态。错误按约定的程序错误身份比较；报告文本或源码位置变化不自动等同于错误消失或新增。

对优化 $\mathcal M_f\Rightarrow\mathcal M'_f$，最低限度的无漏报义务是

$$
\forall\pi\in\Pi_s,\qquad
\operatorname{Err}_s(\pi,\mathcal M_f)
\subseteq
\operatorname{Err}_s(\pi,\mathcal M'_f).
$$

$\Pi_s$ 是该 sanitizer 与 backend 明确支持、且采用相同观测作用域的执行集合；若要比较任意有限前缀或延迟报告，需要另行定义报告时序关系。上述包含关系只表达检测保持；避免新增误报、保持原程序行为及其他可观测性质还需要相应约束。此式是变换与物化共同承担的目标，不是由图可达、结构 verifier 或现有测试自动推出的定理。

## 4. 用于优化的带类型图

对选定的 sanitizer 策略 $s$，从 $\mathcal M_f$ 导出辅助图

$$
G_{f,s}=(\mathsf{PP}_f,E^{D}_{f,s},E^{P}_{f,s}),
$$

其中 $E^D$ 与 $E^P$ 分别记录沿 dominance 方向和沿 post-dominance 方向可传播的候选事实。初始边结合控制流结构与对应方向的 barrier 条件构造；变换后仍保留边的方向语义，但不要求每条边都重新等同于原始控制流的直接支配关系。

Sanitizer 策略还决定访问类型的覆盖关系、Observation 是否可删除、合并或移动，以及哪些边可用于覆盖传播。这些条件彼此独立。例如当前 ASan 策略把读写类型视为同一覆盖级别，仅允许沿 $E^D$ 传播覆盖，且不移动 Observation；当前 TSan 策略使用 $r\preceq_s w\preceq_s rw$，并允许两种方向在各自条件下传播。两者仍需检查区域、属性与执行条件。

记 $E^{\mathrm{cov}}_{f,s}\subseteq E^D_{f,s}\cup E^P_{f,s}$ 为逐边通过当前策略检查的覆盖传播边集。每条边的两端必须属于同层 MAIR，父子图之间没有普通传播边。跨 PP 覆盖可以使用 $(E^{\mathrm{cov}}_{f,s})^+$，但这种可达性**只是候选关系**。只有独立保留的 Observation $o_1$ 对目标 Observation $o_2$ 满足 sanitizer 特定的 $\operatorname{covers}_s(o_1,o_2)$，且相应动态实例的区域与效果确实匹配，$o_1$ 才能作为删除 $o_2$ 的见证。$\operatorname{covers}_s$ 至少要求 Observation 可删除、区域包含、类型覆盖及属性兼容；其中区域包含必须针对被配对的动态实例解释。静态值相同本身不足以跨迭代证明覆盖。

## 5. 已实现变换的抽象边界

一次 pass 的抽象判断记为

$$
P\vdash_s(\mathcal M_f,G_{f,s})
\Rightarrow(\mathcal M'_f,G'_{f,s}).
$$

下列规则只陈述各类变换不能省略的前提。具体静态判定可以比这些语义前提更保守；一次规则应用仍需满足第 3 节的整体正确性义务。

**图收缩（SimplifyGraph）。** 若 $u\,E^D\,v$ 且 $v\,E^P\,u$，两个 PP 的 site 与 guard 具有兼容的执行语义，且 $u$ 的全部 Observation 可移动，则可将 $u$ 的 payload 下沉至 $v$ 并收缩图。跨基本块时，当前实现还要求两端处于同一最内层循环。互有无类型边并不足以允许收缩。

**冗余观测删除（ROE）。** 若独立且保留的 $o_1$ 在同一 PP 内或经允许的覆盖传播到达 $o_2$，并满足 $\operatorname{covers}_s(o_1,o_2)$，则可删除 $o_2$ 这一 Observation。覆盖必须同时处理动态区域包含、访问类型、属性和删除能力。

**同 PP 区域合并（LocationMerge）。** 只有在两个 Observation 属于同一 PP、语义兼容且可合并，并且 $\ell_1\cup\ell_2$ 可在目标处精确表达时，才能用一个 Observation 替代二者。替代 Observation 的类型为 $t_1\sqcup t_2$，并保留必要属性与来源。不能以包含未观测空隙的外包区间代替精确并集；当前 pass 不聚合周期区域。

**共同观测移动（CommonObservationMotion，CLI 名称 `com`）。** 若某个完整控制流分支集合的每条分支均有等价的可移动 Observation，并且带类型的图关系、目标值可用性和执行覆盖均成立，可在共同 PP 合并这些 Observation。提升和下沉分别使用各自方向的事实；任取一部分分支或仅凭共同支配节点不足以成立。

**循环不变观测移动（LIOM）。** 区域在循环中不变只是候选条件。移动还要求源与目标之间有合法的方向关系、目标处可求值，并保持原 PP 的执行条件。移至循环前时需证明适当的必执行关系；移至循环后时，条件执行与零次执行需由 guard 或等价条件处理。

**循环周期观测合并（LPOC）。** 对仿射地址序列 $p+i\,d$，只有在 Observation 可移动、源 PP 每轮的执行次数和总执行次数均可精确确定，并且存在通往出口的合法 post-dominance 与 barrier 关系、出口位置及所需值合法时，才可用相应的周期区域代表其所有动态观测。只有当区域的精确并集为连续区间时，才能改写为连续区域。知道地址可由循环表达式描述，并不等于知道该 PP 实际执行了多少次。

以上是当前 MAIR graph pass 的范围。对象安全性消除尚无独立的 MAIR pass；它不能作为此处已经实现的变换规则。

## 6. 物化边界

MAIR 实例经目标选择与优化后，由 backend 将受支持的 PP 及其 Observation 物化为具体检查。物化前须确认 site 与 guard 合法、区域表达所需的操作数在目标位置可用、来源关联有效，并确认 backend 能发射所需语义；不满足条件的 PP 不能仅因 MAIR schema 可表达就被视为可物化。

因此，MAIR 的表达能力、各优化 pass 的适用域、具体 sanitizer 的目标选择与 backend 支持域分别定义。完整正确性要将这些阶段组合起来，在第 3 节的动态执行上验证；本文只给出这一义务及当前抽象的边界。
