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

每个优化阶段按子图优先顺序执行，随后处理父图。普通冗余删除、位置合并和公共 Observation 移动继续使用本层 MAIR。它们不能沿父子图链接复用检查。循环不变量移动和周期合并可以显式导出到父图中的循环边界，嵌套导出每次只跨一层，逐层重新证明。内层周期合并会把新的区间观察放到父图的循环出口。`default<O1>` 把位置合并、不变外提和周期合并编成一组，对每层图依次跑完再处理父图（见“`default<O1>` 的顺序”），父层的这三步因此能看到子循环刚导出的观察，并可逐层再汇总（见“外层循环再汇总”）。`default<Og>` 仍是各自整树遍历：父层第一次不变外提此时已经结束，所以它在周期合并后再跑一次不变外提，让外层循环重新证明这条观察。

不变量移动按本图 ProgramPoint 和 Observation 的原顺序逐项计划并立即提交，每次实时查找父图目标。提交只删除当前源 Observation 并更新父图，不改本图其他候选、拓扑或 LLVM 分析，因此不需要从头重扫；若以后改变这些前提，必须重新检查单遍遍历的等价性。

`default<O1>` 在首次不变量移动前先做冗余删除。循环展开可能在同一条件块中留下多次相同访问；先消除重复观察，才能在下沉时只保留一个来源 guard。若先逐项下沉，来源不同会生成不同 guarded ProgramPoint，随后冗余分析也不会把它们作为无条件覆盖来源。冗余删除只保留原有覆盖见证及其来源，不拼接被删除观察的 provenance，因此不改变保留检查的源位置规则。消冗余仍遵守既有层次、覆盖和 sanitizer 规则。`default<Og>` 不改变既有优化顺序。

两个 Observation 互相覆盖时（层次相同，例如 ASan 下的读和写，或 TSan 下的两个读；`A` 支配 `B` 且 `B` 后支配 `A`），保留先执行的 `A`，删掉 `B`，与读写无关。层次不同时只有高层次的一方能覆盖，规则不变：泛型配置和 TSan 下，先读后写仍保留写。跨 ProgramPoint 消冗余按 Dom 边的后序访问 ProgramPoint，即每个 ProgramPoint 都先于沿 Dom 边能到达它的 ProgramPoint 被检查；先检查的被覆盖者删除后不再作为见证，所以支配者留下（`doInterROE`，`src/instrumentation/MAIR/Transforms/RedundantObservationEliminationPass.cpp`）。图的链表顺序跟随基本块布局，不一定是支配顺序，不能直接用。顺序只决定互相覆盖的两者谁留下，不影响正确性：被删除的 Observation 在被删时总有一个尚未被删除的见证（`ExcludedSources`），与访问顺序无关。同一 ProgramPoint 内的消冗余按稳定排序保留链表中靠前的一个；`default<O1>` 的移动 pass 会把区域和层次相同的 Observation 合并（保留全部来源），只有不合并的 `default<Og>` 才会在同一 ProgramPoint 留下多个，顺序是循环子图的链表顺序。测试：`test/MAIR/TestCases/redundant-observation-elimination.ll` 的 `earlier_*`、`later_write_covers_earlier_read`、`dominator_laid_out_later`，单元测试 `AddressSanitizerKeepsTheDominatorOfMutuallyCoveringAccesses`、`ThreadSanitizerKeepsTheLaterWriteOverADominatingRead`。

同一 ProgramPoint 内，常量偏移、常量大小的连续区间按区间扫描消冗余；同一对象的其余 Observation（符号偏移、表达式大小、periodic、masked）排在其常量区间之后，组内两两用 `Observation::covers` 判断（`eliminateNonConcreteRun`，`RedundantObservationEliminationPass.cpp`）。没有这一步时，TSan 下 `p[i]++` 和 `static _Thread_local long t; t += 1;` 的读和写落在同一 ProgramPoint，地址是符号，读留了下来，比 native（只有 `__tsan_write*`）多一个回调。被删者从后往前取，见证从前往后找，所以互相覆盖的两者留下链表中靠前的一个，与常量区间一致；用过的见证此后不再删除，因为跨原子性的覆盖要求两边都可合并，覆盖不一定传递，每个被删者自己的见证都必须留下。组内是平方复杂度，与位置合并的符号区间一样；同一对象的非常量 Observation 在合并之后通常只有个位数。测试：`redundant-observation-elimination.ll` 的 `same_offset_levels`、`same_offset_equal_reads`，单元测试 `RedundantObservationKeepsFirstEqualSymbolicObservation`，sanitizer 级 `test/MAIR/TestCases/tsan/mair-read-modify-write.c`（MAIR 与 native 输出一致）。

**常量差覆盖。** `AbstractAccess::contains`（`src/instrumentation/MAIR/MAIRRegion.cpp`）比较两个常量大小、偏移不全是常量的连续区间时，先求被覆盖者起点相对见证起点的距离 $\Delta$：偏移是同一个符号时 $\Delta=0$；否则若调用者给了 ScalarEvolution，用 `constantAddressDelta`（`getAddressSCEV` 后 `computeConstantDifference`，与位置合并同一实现）求常量差。包含条件是 $0\le\Delta$ 且 $\Delta+|o_2|\le|o_1|$，用 128 位算术。只有 ROE 传 SE（`RedundantObservationEliminationPass::run` 从 `LoopAnalysis` 取，方式同 `LocationMergePass`），intra 和 inter 都用；`Observation::covers`、`RedundantObservationInfo::isCovered` 的 SE 参数默认为空，其他调用者行为不变。区域的对齐（`getAlign`，见 `docs/mair-region-alignment.md`）只描述起点地址，不参与包含判断，和它不参与 `operator==` 一致。跨 ProgramPoint 时还有一条保守条件：见证必须在目标不变的每个被迭代环中也不变，即 `closedCycles(Source) ⊆ closedCycles(Target)`（`RedundantObservationInfo::isCovered`）。

正确性。`computeConstantDifference` 返回常量，说明两个地址的 SCEV 在消去相同项后只差常量：两边由同一组叶子（SCEVUnknown，即 SSA 值）和同一组 AddRec 组成。所以只需证明：见证执行时和目标执行时，每个叶子取同一个值，每个 AddRec 所在循环处在同一轮。原有的同符号覆盖证明的是同一件事，对象是地址的直接操作数；这里对象换成了它们的传递操作数。叶子的定义支配地址的计算（操作数支配使用；SCEV 穿过 phi 时只取支配该 phi 的入值，如 LCSSA phi、同值 phi、select 形 phi），AddRec 对应循环 header 的 phi，所以下面的论证对叶子和直接操作数一样成立。

- Dom 引理：`W` 支配 `T`，`x` 的定义支配 `W`。在 `T` 之前最后一次执行 `x` 的定义之后，控制流一定又经过了 `W`。否则把“从入口第一次到达 `x` 的定义”（这段不经过 `W`，因为 `x` 的定义支配 `W`）和“从最后一次定义到 `T`”拼起来，就得到一条不经过 `W` 到 `T` 的路径。所以 `W` 最后一次执行时看到的 `x` 就是 `T` 看到的 `x`。对 AddRec，把 `x` 换成循环 header：同层的 `W`、`T` 处在所在循环的同一轮；地址用到已退出子循环里的值时，该值的定义支配循环外的使用，自然循环中它一定在最后一轮执行，两边都取最后一轮的值。
- PostDom：沿用已有条件，两端在同一最内层循环（`sameLoopForPostDom`），且边不迭代环时两端之间没有开始新一轮迭代，支配目标的定义不会在两者之间重新执行。
- Iterated：边迭代环 `C` 时，只对在 `C` 中不变的目标开放。若目标在 `C` 中不变（直接操作数都定义在 `C` 外），其所有传递操作数也在 `C` 外：取一条先在 `C` 外走到 `C` 的某个入口、再在 `C` 内走到目标的路径（环强连通，这样的路径存在）；直接操作数的定义支配目标，只能在 `C` 外的前缀上，而传递操作数的定义支配直接操作数的定义，也在这段前缀上。保守条件要求见证同样在 `C` 中不变，于是两边的叶子都不随 `C` 迭代，也没有 `C` 的 AddRec，绕环不改变两个地址。不加这条条件时，见证可能用环内定义、SCEV 上不变的值（如 `iterated_pdom_invariant` 里在环内计算的 `p[k-1]`）覆盖环外不变的目标；这仍然正确，但论证要多一步“SCEV 不变”，收益据分诊报告在 e2e 中为 0，所以不做。
- 不可约环：LLVM 不为它建 `Loop`，SCEV 不生成它的 AddRec，环里的 phi 是 SCEVUnknown 叶子，上面的叶子论证直接适用。
- 残余风险（nsw/nuw）：常量差可能依赖无符号或有符号不溢出的标志，例如 `sext(i + 1) - sext(i) = 1` 需要 `add nsw`，或 inbounds GEP。标志不成立时该值是 poison，经它的访问在源程序里是 UB。位置合并的 `constantAddressDelta` 有同样的前提，LLVM 自身的优化也依赖它。

例外与未覆盖：一边偏移是常量 0、另一边是以 phi 为基址的符号指针时，前者用不透明的基址，后者 SCEV 展开为 AddRec，求不出常量差（保守遗漏）；表达式大小的区间和 periodic 区域不走常量差。测试：`redundant-observation-elimination.ll` 中正例 `same_block_levels`（同一 ProgramPoint 内也生效）、`same_symbol_narrower`、`struct_cross_block`、`scaled_cross_block`、`two_equal_geps`；负例 `negative_delta_and_oversize`（$\Delta<0$、超出大小）、`no_constant_delta`（无关下标；不带 nsw 的 32 位加法可能回绕，求不出常量差）、`iterated_pdom`（目标随迭代变化）、`iterated_pdom_invariant`（只被保守条件拦下：去掉条件后该用例失败）。

**表达式大小的包含。** 周期合并为每个循环各建一棵长度表达式，两个循环遍历同样的 `n` 个元素时，得到的是结构相同、节点不同的两棵树。`AbstractAccess::operator==` 按节点身份比较 SizeExpr（合并和移动用它判断结果是哪一边，不改），所以 `contains` 对带 SizeExpr 的区间单独比较：基址、偏移相同，且 `MAIRScalarExpr::operator==`（逐层比较种类、类型、常量、LLVM 值和操作数）判定两棵树结构相同，即为包含。叶子是同一批 LLVM 值，和同符号偏移的覆盖一样依赖“两端取同值”，由同一套可达规则保证。只认相等，不比较大小（例如 `n*8` 与 `n*4`），也不和常量大小互相比较。效果：`if (n <= 0) return; for … p[i] = i; for … s += p[i];` 在 `-O1` 下两个循环出口各有一个区间，前者支配后者，ROE 删去后者，ASan 只剩一个 `__asan_storeN`，TSan 只剩 `__tsan_write_range`。`-O2` 下这种形态少见（分诊报告：守卫被 jump threading 合并、循环被向量化）。测试：单元测试 `AbstractAccessContainsStructurallyEqualSizeExpressions`，sanitizer 级 `test/MAIR/TestCases/{asan,tsan}/mair-repeated-range.c`。

周期合并也直接按本图 ProgramPoint 的原顺序遍历。聚合目标属于父图，提交不增删本图 ProgramPoint，因此不需要快照；内层仍先递增迭代器再删除当前 Observation。来源指针查询使用 LLVM 的 `getLoadStorePointerOperand`，仅接受 load/store，并保留空指针检查。

优化输入统计按每次本层 pass 调用累计；Observation 导出后再次作为父层输入，会再次计入该统计，这不表示重复物化。`print<stat>` 则汇总当前整树的 ProgramPoint 和 Observation 数量。

宿主 LLVM 的 `-stats` 在 Release 构建里不会打印。要看每个 pass 实际改了什么，用 `-mair-stats`。`xclang -mair-stats` 会把它转成 `-mllvm -mair-stats`；`mair-opt -mair-stats` 直接认这个选项。每个编译单元在 stderr 打一块汇总，不按函数展开，避免 C++ mangled 名字淹没构建日志。`default<O1>` / `default<Og>` 里重复出现的 pass 带 `#1`、`#2`，所以第二次不变外提不会和第一次混在一起。`in` 仍按每次本层调用累计；比较效果看 `preheader`、`exit`、`range`、`periodic`、`merged`、`intra`、`inter`、`hoisted`、`sunk`。

`lpoc` 和 `liom` 行还说明为什么有 Observation 留在循环里（每次本层调用各计一次）。`lpoc` 只计地址随循环变化的 Observation，`liom` 只计循环不变的，各按下列顺序取第一个不满足的条件：`rej-barrier`（循环里有 barrier），`rej-nonaffine`（地址不是本循环的仿射递推，仅 `lpoc`），`rej-cond`（来源不是每轮恰执行一次，含 `liom` 跳过的带 guard 的 ProgramPoint），`rej-multiexit`（不止一个 exiting block 或出口块），`rej-nocount`（既没有 SCEV 算出的次数，也读不出出口处的次数，仅 `lpoc`），`rej-other`（其余，如原子访问、非连续区域、起点或次数无法在出口表达、出口不后支配来源）。分类只在 `-mair-stats` 下计算，不影响优化。`exit-trip` 是 `range`/`periodic` 中用出口处归纳变量求次数的个数（见“数据相关的出口”）。实现在 `classifyRejection`（`LoopPeriodicObservationCombinePass.cpp`、`LoopInvariantObservationMotionPass.cpp`），测试 `test/MAIR/TestCases/mair-stats-loop-rejections.ll`，以及 `loop-early-exit.ll` 和 `asan/mair-early-exit-oob.c` 的 `STATS`（后者含 `rej-barrier`）。

sanitizer 编译还会多一行 `select`，在优化之后统计 lowering 去向：`in` 是非 call-like 的终态 Observation，`lowered` 交给 MAIR 物化，`native` 退回 sanitizer 自己逐访问插桩；`multi`（多源，合并产物）、`symbolic`（非常量偏移/大小或非连续区间，含 periodic）、`vector`、`shared`（来源同时被一个退回的 Observation 持有）说明退回原因，其余退回原因是 sanitizer 自身判定（如 ASan 安全栈访问、TSan 未逃逸 alloca）。`fallback` 是退回 native 的来源指令数。实现在 `selectMaterializableObservations`（`src/instrumentation/MAIR/Materialization/MAIRMaterialization.cpp`），测试 `test/MAIR/TestCases/asan/mair-select-stats.c`。这几个原因字段只描述 Observation 的形状；ASan 已能 lowering 多源、符号区间、periodic 和固定宽度向量（见“ASan 的 lowering”），在 ASan 下它们只剩 masked 区域和来源本身被 ASan 拒绝的情况。

统计汇总按函数首次出现的顺序遍历各函数的 pass visits，以 pass 标签首次出现的位置决定行顺序；不是全局事件时间顺序。汇总阶段一次累计，保持 ordinal、字段顺序及只含 `in` 的行，全零行不打印。

循环 barrier 摘要来自构图时的 LLVM 指令，并包含子循环。导出 Observation 不会抹掉摘要。当前最小规则是：提升、下沉和周期合并都要求循环中正向、反向均无 barrier。任一迭代中的 barrier 都阻止相应导出，不需要逐动态回边路径查询。这是保守条件，并未放宽既有必执行、SSA 可用性、精确计数、affine recurrence 或 guard 条件。提升到 preheader 的必执行条件是：来源块支配循环的每个 exiting block，即任何出口都只能在来源块执行之后走到（`MAIRLoopInfo::canHoistToPreheader`，`src/instrumentation/MAIR/Analysis/LoopAnalysis.cpp`，例子写在该函数的注释里）。这个条件蕴含“来源块后支配 preheader”，所以不再单独查后支配树；反过来不成立，控制流可以不经过来源块就离开内层循环，再从外层循环绕回来。没有出口的循环改为要求来源块支配每个 latch。不满足条件的访问仍可用“来源执行过”的 guard 下沉到唯一出口；多出口循环需要在每条出口边放带 guard 的副本并改写 CFG，尚未实现。ASan 允许搬动普通 Observation，检查可以落在实际访问之后，因此也和 TSan 一样沿 PostDom 边传播覆盖。前提是后面的检查一定会执行：ASan 的反向 barrier 除了正向 barrier，还包括任何不保证把控制流交给后继的指令（可能抛异常或不返回的调用；哪些调用是 barrier 见下节）。原子访问仍然不能搬。

带“来源访问执行过”条件的下沉，以本次循环调用的 preheader 为 guard 重置位置。嵌套循环的下一次调用不会继承上一次调用的 true 状态。整树统一生成物化计划，统一 prepare/apply；完成后整树都进入 consumed 状态。

物化必须在 sanitizer emission 前完成全部拆边、guard 和观察参数 lowering。sanitizer 可自行拆 CFG，MAIR 无法同步维护这些修改后的局部分析，因此不能交错执行后续 guard 更新。第一阶段保留 DTU/LI，pending record 自有 lowered observations，并保存 plan unit 指针和稳定指令插入点；第二阶段按原顺序从指令的当前基本块重建 builder，仅在同步 emit 时构造 ArrayRef。若后来的 guard 拆分共享 anchor，早先 pending 的插入点改指向拆分前块的新条件 terminator，保持原先 emission 在该 guard 之前。backend 必须保留后续插入指令和已物化参数；第一遍之后不再消费局部 CFG 分析。

物化复用本次 preflight 建立的 DT/LI，通过 LLVM 拆边接口增量维护；不建立未被查询的 PDT。backend 的 prepare 必须保持 IR 不变，失败时不能留下 CFG 修改。

MAOpt 的调用者通过 `allProgramPoints()` 读取当前图，不维护额外的 Observation 快照。sanitizer 在优化前记录来源集合；ASan 先按读写开关过滤，再记录来源。优化后先收齐不能由 MAIR 物化的来源，再排除存在同源冲突的候选。已处理来源是优化前集合减去原生路径来源，不能只取最终候选的来源，否则被冗余删除的检查会重新插入。call-like Observation 保持原生 lowering。

内层范围起点复用已有 LLVM Value 时，要求 SCEV 相等、目标位置可用、类型匹配。**obs 的优化不受 poison 与否影响**（老板决定，2026-11-17）：即使候选指针值是 poison，native ASan 也照样检查这个指针，所以 MAIR 可以拿它表示被合并的区间。因此候选指令带 poison flag（`nsw`/`nuw`/`inbounds` 等）、需要丢弃这些 flag 才能等于起点 SCEV 时照样接受；优化阶段不修改原 LLVM IR，flag 原样保留。`ScalarEvolution::canReuseInstruction` 仍然调用，但只用来拒绝算出的值可能与 SCEV 不同的候选，例如 `or disjoint`（操作数有公共位时它不是加法，丢掉 flag 也不等于 SCEV 的加法），丢弃列表不再检查（`canReuseValue`，`src/instrumentation/MAIR/Transforms/LoopPeriodicObservationCombinePass.cpp`）。这取代了 `.archive/review_loop_ranges_20261110_1447/review.md` 中 F3 的保守结论（"需要丢 flag 则拒绝候选"）；F4 要求的 `isSCEVable` 与类型过滤保留。`loop-start-reuse.ll` 覆盖带 poison flag 的候选被复用、`or disjoint` 候选被拒绝、非 SCEV 类型。

符号 trip count 加符号起点时同样生成连续区间 `[base + start, + count * size)`（以前只接受常量起点，符号起点退成 periodic）。单元测试 `PreservesRangeBoundarySelection` 的 "symbolic count symbolic start"、`test/MAIR/TestCases/asan/mair-loop-range.c` 的 VEC 前缀覆盖。

periodic 回退的起点若是外层循环的 AddRec，`SCEVExprConverter` 不能转换。此时先找一个算出该 SCEV 的 IR 值作叶子（`findValueForSCEV`），找不到再用访问 GEP 的指针操作数加一段可转换的差值表示（如 `%col + 4`，循环展开后的 `B[k+1][j]` 就是 `%col + 4n`）（`representPeriodicStart`）。

### 外层循环再汇总

内层汇总出的区域在父层还能再汇总成一个连续区间，前提是父层各次迭代的足迹首尾相接（`buildNestedRangeAggregate`、`buildNestedPeriodicAggregate`、`buildAbuttingRange`）。父层的 AddRec 取自区域的起点：区间取地址 SCEV，periodic 区域把起点表达式还原成 SCEV（`getSCEVOf`，叶子是 LLVM 值，所以内层留下的外层归纳变量会还原成外层 AddRec）。区域的大小、stride、trip count 也还原成 SCEV，必须在父循环中不变。

- **长度为表达式的区间**：每次迭代覆盖 `[R(i), R(i) + S)`。父层步长等于 S 时，各段首尾相接，并集是 `[R(0), + Count * S)`。例：`A[i][k]` 的行 `[A + 4ni, + 4n)`，i 每次前进 4n。
- **periodic 区域**：起点 `s(j)`、stride S、access size a、trips T。父层每次把起点推进 a，且父层的 Count 个迭代恰好填满一个 stride（`Count * a == S`）时，第 k 轮的 Count 段在 `[s(0) + k * S, + S)` 内首尾相接，各轮之间也首尾相接，并集是 `[s(0), + T * Count * a)`。例：`B[k][j]` 的列（start `B + 4j`、stride 4n、size 4、n 轮），j 每次前进 4、n * 4 = 4n。条件不满足（只取前 m 列、隔列读取等）时保留原 periodic 区域，不合成外包区间；二维或更一般的 periodic 形式尚未实现。
- 步长与大小按指针索引宽度取模比较：地址本来就按这个宽度回绕，第 i + 1 段的起点恰好是第 i 段的终点，当且仅当两者在这个宽度下相等。总长度按无符号乘积在足够宽的整数类型里计算，不会回绕（`getUnsignedProduct`）；总长度为常量时直接写成常量大小。
- 区间从第一段的起点开始，继承内层区域的对齐（`docs/mair-region-alignment.md`）。
- 不变外提也接受长度为表达式的区间：表达式里的 LLVM 值都在循环中不变即为不变，都在目标处可用即可移动（`MAIRLoopInfo::isLoopInvariant`、`isRegionAvailableAt`）。矩阵乘法中 B 的整块区域与 i 无关，由此提到 i 循环之前。

例：`C = A * B`（`test/MAIR/TestCases/loop-matmul.ll`）在 `default<O1>` 后只剩 3 个 observation：C 的区间写，A、B 的区间读，常量 N 与符号 n 都一样。负例见 `test/MAIR/TestCases/loop-nested-symbolic.ll`（行间有 pitch、行长随外层变化、只取部分列、隔列）。sanitizer 侧：`test/MAIR/TestCases/{asan,tsan}/mair-matmul.c`（每个函数恰好 3 个区间回调，无逐元素回调），`asan/mair-matmul-oob.c`（C 最后一个元素越界仍报告），`tsan/mair-race-matmul.c`（另一线程写 A 的一个元素仍报竞争）。

## Barrier

barrier 是构图时按方向断边的指令（`MAIRBarrierAnalysis`，各 sanitizer 的 trait 在 `src/instrumentation/llvm23/{Address,Thread}Sanitizer.cpp`）。正向 barrier 阻止先执行的 Observation 代表后面的访问（Dom 方向），反向 barrier 阻止后执行的代表前面的（PostDom 方向）。它同时决定 Dom/PostDom 边（`MAIRGraphBuilder.cpp`）和循环导出（上节：提升、下沉、周期合并要求循环内两个方向都没有 barrier），所以冗余删除、公共 Observation 移动和合并都受它影响。

**基准**：一条调用对本 sanitizer 的检测状态没有可观察的影响，就不算 barrier。

- ASan 的状态是 shadow：调用不 free，不 poison 或 unpoison，也不能经同步看到别的线程做的这些事。
- TSan 的状态是 happens-before、shadow 和线程状态：调用不建立 happens-before，不 free 或重置 shadow，不切换 fiber，不改 ignore 状态。
- 反向还要求调用一定返回（`isGuaranteedToTransferExecutionToSuccessor`，即 `willreturn` 且 `nounwind`）：后面的检查要代表前面的访问，就必须执行到。

**规则 1（严格）**：同时带 `nosync` 和 `nofree` 的调用不是正向 barrier；一定返回时也不是反向 barrier（`TsanMAIRBarrierTrait::isTransparentCall`、`AsanMAIRBarrierTrait::mayChangeShadow`）。

- `nosync` 保证不建立 happens-before，也看不到别的线程的 free 和 poison；`nofree` 保证自己不 free（TSan 把释放的内存标成 freed，ASan 把它 poison）。分配会重置 TSan 的 shadow、unpoison ASan 的 chunk，但 LLVM 从不给 malloc、`operator new` 这类函数标 `nosync`，所以它们仍是 barrier。
- 这两个属性的来源：intrinsic 的默认属性（`DefaultAttrsIntrinsic`，`llvm/include/llvm/IR/Intrinsics.td`）；`memory(none)` 且不是 convergent 推出 `nosync`，只读推出 `nofree`（`inferAttributesFromOthers`）；FunctionAttrs 对精确定义的推断，遇到未知调用、inline asm、非 unordered 原子或跨线程 fence 就不推。`BuildLibCalls` 不给库函数设 `nosync`。没有找到带 `nosync` 却会同步的调用；属性推断错了在 LLVM 里本就是 UB。sanitizer 的接口函数都是没有属性的外部声明，因此都是 barrier，包括 `__asan_poison_*`/`__asan_unpoison_*`、`__sanitizer_annotate_contiguous_container`、`__sanitizer_annotate_double_ended_contiguous_container`、`__sanitizer_start_switch_fiber`、`__tsan_acquire`/`__tsan_release`、`__tsan_mutex_*`、`__tsan_switch_to_fiber`、`__tsan_ignore_thread_*`、`Annotate*`。
- TSan 原有的单侧规则不变：release 是正向 barrier，acquire 是反向 barrier。memintrinsic 本身是 Observation，两个 sanitizer 都不把它当 barrier。ASan 仍只看可能写内存的调用：只读调用不能 free，也不能改 shadow。

**规则 2（ASan 例外）**：ASan 自己会改写成 shadow 更新的调用，即使带 `nosync` 和 `nofree`，也仍是两个方向的 barrier：

- `llvm.lifetime.start`/`llvm.lifetime.end`：use-after-scope 的 unpoison/poison；
- `llvm.stackrestore`：ASan 在它前面插 `__asan_allocas_unpoison`。

逐一核对了 ASan 处理的其他 intrinsic（`FunctionStackPoisoner::visitIntrinsicInst`、`getInterestingMemoryOperands`、`instrumentMemIntrinsic`）：`localescape` 只影响栈帧布局；`masked.*`、`vp.*` 是访问本身；memintrinsic 改写成 `__asan_mem*`，只检查不改 shadow；`get_dynamic_area_offset`、`asan.check.memaccess` 由 ASan 自己生成。动态 alloca 在 `alloca` 指令处 poison，不是调用。TSan 不跟踪 lifetime，`lifetime.*` 和 `stackrestore` 对 TSan 不是 barrier。

TODO（研究）：lifetime 是 ASan 循环里最大的单项阻挡。只对 ASan 会做作用域插桩的 alloca（`UseAfterScope` 打开且 `isInterestingAlloca`）才把它当 barrier，或者按对象生效，能放开更多循环。

**规则 3（启发式，名字白名单）**：SanitizerCoverage 在插桩点调用的回调 `__sanitizer_cov_trace_*`、`__sanitizer_cov_load*`、`__sanitizer_cov_store*`，对两个 sanitizer 在两个方向都不是 barrier，并视作一定返回（`mair::isSanitizerCoverageCallback`，`src/instrumentation/MAIR/MAIRBarrierAnalysis.cpp`）。

- 依据：它们的实现（libFuzzer 的 `src/runtime/lib/fuzzer/FuzzerTracePC.cpp`，都是 `ATTRIBUTE_NO_SANITIZE_ALL`；`sanitizer_common/sanitizer_coverage_libcdep_new.cpp` 的弱定义，多数为空）只更新覆盖率表，不加锁，不 free，不碰 shadow。SanitizerCoverage 声明它们时不带任何属性，规则 1 放不过它们。sqlite3、re2 的 e2e 用 `-fsanitize=fuzzer-no-link`，sancov 在 MAIR 之前插桩，每个比较前都有一个 `__sanitizer_cov_trace_*cmp*`；循环里残留的逐次检查有 77–92% 是被它们挡住的。
- 假设：用户自己实现的回调（自定义覆盖率、trace-loads/stores）同样不同步、不 free、不改 shadow，并且返回。
- 用白名单，不用“`__sanitizer_` 前缀加排除表”。这个前缀下有状态影响的函数很多，还会随 compiler-rt 增加：容器注解、`__sanitizer_copy_contiguous_container_annotations`、fiber 切换、`__sanitizer_syscall_pre/post_*`（TSan 里做 acquire/release 和范围访问）、`__sanitizer_unaligned_load/store*`（TSan 里记录访问）等。排除表漏掉一项会漏报，白名单漏掉一项只是少一些优化。其他 `__sanitizer_*`（包括 `__sanitizer_cov_dump` 这类非插桩点回调）、`__asan_*`、`__tsan_*`、`__ubsan_handle_*` 都仍按规则 1 判断，即都是 barrier。

**规则 1 覆盖的常见 intrinsic**（只是说明，没有单独的代码路径）：`dbg.*`、`pseudoprobe`、`assume`、`sideeffect`、`prefetch`、`experimental.noalias.scope.decl`、`invariant.start/end`、`launder/strip.invariant.group`、`var/ptr.annotation`、`annotation`、`codeview.annotation`、`donothing`、`expect`、`objectsize`、`threadlocal.address`、`fake.use`、`allow.ubsan.check`、`allow.runtime.check`、`stacksave`、`va_start/va_end/va_copy`，以及算术类（`smax`、`umin`、`fma`、`bswap`、`fshl` 等；这些是 `memory(none)`，ASan 原来就不当 barrier）。不在其中的：`instrprof.*`（没有默认属性；PGO 插桩的 lowering 在 sanitizer 之前）、`trap`/`ubsantrap`（不返回）、`experimental.guard`。

测试：

- `test/MAIR/TestCases/tsan/mair-barrier-calls.c`：smax、fma、assume、lifetime（加推断为 `nosync nofree willreturn` 的用户函数）的循环汇总成 range；外部调用的循环逐次记录；`-fsanitize=fuzzer-no-link` 下 `sum_cmp` 也汇总。
- `test/MAIR/TestCases/{tsan,asan}/mair-barrier-attributes.ll`：属性矩阵。`nosync nofree willreturn` 和 sancov 回调放行；缺 `willreturn` 时正向放行、反向和循环被挡；缺 `nofree`、外部调用、`__tsan_release`、`__sanitizer_annotate_contiguous_container` 被挡。
- `test/MAIR/TestCases/tsan/mair-race-barrier-calls.c`：带 smax 的循环汇总后，与其中一次迭代的竞争仍报告。
- `test/MAIR/TestCases/asan/mair-barrier-calls.c`：assume、prefetch、`nosync` 用户函数和 sancov 的循环检查一次；lifetime 循环和 VLA（`stackrestore`）循环仍逐次检查。
- `test/MAIR/TestCases/asan/mair-barrier-runtime.c`：跨 `lifetime.end` 的 use-after-scope、跨容器注解的 container-overflow 仍报告。

## `default<O1>` 的顺序

`buildMAIRGraphO1Pipeline`（`src/instrumentation/MAIR/MAIRPasses.cpp`）依次运行：

```
simplifygraph#1 → roe#1 → [locationmerge#1, liom#1, lpoc] → liom#2 → locationmerge#2 → roe#2 → simplifygraph#2
```

方括号内的三个 pass 编成一组（`addHierarchyPass` 的多 pass 形式）：对每层图依次跑完这三步，再处理父图，子图先于父图。其余 pass 各自做一次整树遍历（子图先于父图）。顺序依据 e2e 的 `-mair-stats` 和 `instrumented_mops`：

- `roe#1` 在最前，原因见上文（下沉时只保留一个来源 guard）。把位置合并提到它前面试过：re2 ASan 少 81 个检查，但 7zip ASan 多 112、sqlite3 ASan 多 8；合并还会拼接来源，ASan 因多源退回 native 的 Observation 变多（sqlite3 `multi` 342 → 463），所以不采用。
- `locationmerge#1` 在不变外提和周期合并之前。循环体里的 `p[2i]`、`p[2i+1]` 先合并成一个 8 字节、步长也是 8 的访问，周期合并得到一个连续区间；先做周期合并只能得到两个 stride 8、size 4 的 periodic 区域。7zip ASan 中周期合并的 `range` 从 125 增至 438，`periodic` 从 3282 降至 1060。测试：`test/MAIR/TestCases/o1-pipeline.ll` 的 `o1_location_merge_before_periodic_combine`。
- 位置合并、不变外提和周期合并逐层成组：父层的位置合并能合并子循环刚放到边界上的观察（例如展开后的四列交错 periodic 区域合成一列），不变外提能把内层汇总出的不变区间提出去，父层周期合并再汇总它们（见“外层循环再汇总”）。分组前，内层汇总的结果要等整树的 `liom#2`/`locationmerge#2` 才处理，父层周期合并已经跑过。在 7zip（320 个 TU）和 sqlite3 的 `-O2` IR 上，分组后终态 Observation 数不变，`liom#2` 和 `locationmerge#2` 不再有改动（它们的工作在组内完成）。两者暂时保留，是否删去待定。测试：`test/MAIR/TestCases/mair-stats.ll`（外层的 `liom#1` 提升内层合成的区间）、`loop-matmul.ll` 的 `mm_const_unrolled`。
- `liom#2` 原因见上文；分组前新顺序下 7zip ASan 仍有 12 个提升，分组后为 0。
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

## 数据相关的出口

`while (a[i] >= 0) i++`、`while ((*d++ = *s++) != 0)` 这类循环按读到的数据决定何时退出，SCEV 算不出 backedge-taken count（BTC）。归纳变量在出口处仍记着跑了多少轮：表头的归纳变量 `IV = {Start,+,Step}` 在退出的那一轮取值 `Last = Start + BTC * Step`，所以 `BTC = (Last - Start) / |Step|`（`Step < 0` 时为 `(Start - Last) / |Step|`）。LPOC 在 SCEV 给不出精确次数时改用这个表达式（`MAIRLoopInfo::getExitValueExecutionCountSCEV`，`src/instrumentation/MAIR/Analysis/LoopAnalysis.cpp`），其余流程（连续区间或 periodic 区域、放到唯一出口）与精确次数相同。老板 2026-11-17 决定：只要能给出合法的 trip count 表达式，汇总就合法，次数可以是运行时值（`SPEC.md` 第 5 节）。

条件（全部满足才用；任一不满足就留在循环里，`-mair-stats` 记为 `rej-nocount` 或更前的原因）：

- 来源每轮恰执行一次：always guard、锚定 site、直接属于该循环、支配唯一 latch、循环体内无不可归约环（与精确次数相同）；循环里正反向都没有 barrier，唯一出口后支配来源（`canSinkToExit`）。
- 只有一个 exiting block，它直接属于该循环（不在子循环里），且只有一个出口块。
- 来源与 exiting block 之间有支配关系。来源支配 exiting block（在退出判断之前），退出的那一轮也执行了它，次数为 `BTC + 1`，区域为 `[S, addr_last + size)`；exiting block 支配来源（在判断之后），退出的那一轮没执行它，次数为 `BTC`，区域为 `[S, addr_exit)`，0 轮时为空。两者都没有则不汇总。次数都比 BTC 宽一位，与精确次数的处理相同。
- 归纳变量取表头的 phi：SCEV 为本循环的仿射递推、步长是非零常量；整数或指针类型，优先整数（表达式更简单）。`Start` 是它从 preheader 来的值；`Last` 是它在出口处的值，有 LCSSA phi 就读 LCSSA phi，否则直接读表头 phi（表头支配出口，`isValueAvailableAt` 检查可用性）。表头 phi 的定义块支配 exiting block，所以出口处读到的一定是退出那一轮的值。指针归纳变量用 `ptrtoint` 相减。
- 归纳变量不回绕，`Last - Start` 才精确：SCEV 证明了 no-self-wrap、no-signed-wrap，或（步长为正时）no-unsigned-wrap。`size_t i` 这类没有 wrap 标志的计数器，若至少和地址一样宽、每轮步长不超过访问地址的步长、地址步长不超过一页（4096 字节），也接受：回绕之前访问必须扫过整个地址空间，必然先碰到未映射的页（例如第 0 页）而崩溃，走不到出口。地址步长超过一页时不接受（测试 `huge_stride`）。

不插计数器，也不改 IR：sanitizer 在 `-O2` 的循环优化之后、LSR 之前运行，表头已有整数或指针归纳变量。优化阶段只引用已有的值（SCEVUnknown → `MAIRScalarExpr::LLVMValue`）；物化时只在出口处生成 `sub`/`udiv`/`zext` 一类的计算，循环体不变，backend 的 `prepare` 失败时 IR 也不变（单元测试 `MAIRMaterialization.LowersExitTripCountAtTheExitOnly`）。为了表示指针差，MAIR 表达式新增 `PtrToInt`（指针转为同宽整数，lowering 成 `ptrtoint`，verifier 检查宽度）；SCEV 到 MAIR 的转换也因此接受 `ptrtoint`，`for (p = b; p != e; ++p)` 这类精确次数里含 `ptrtoint` 的循环（分诊报告中 7zip 约 199 个）随之可以汇总。

### 已接受的行为

- 检查推迟到出口，落在全部访问之后。ASan 在出口一次检查整个区间，报告的大小是整个区间（如 `WRITE of size 48`），地址是第一个坏字节，位置取最先执行的来源；TSan 在出口记录一次 range 访问，与已有的循环汇总相同。
- 循环在到达出口之前就因越界访问而崩溃（SEGV），崩溃本身就是可诊断的信号，不要求先报告（老板决定，2026-11-17）。
- 对 ASan 的写循环，这意味着越界写在检查之前就已发生，且越界长度由数据决定、没有上界：精确次数的 LPOC 越界长度受次数约束，而 `while (*s) *d++ = *s++` 可能一直写过相邻 chunk 的头部、别的对象，甚至栈帧（含返回地址），直到出口或 SEGV。只要循环正常退出，出口处的检查仍会报告并终止程序（默认 `halt_on_error=1`）；循环里没有调用（有调用就是 barrier，不会汇总），被破坏的数据在报告前不会被分配器或返回指令使用。若越界写改写了循环自己要读的数据（例如源串与目标重叠），循环可能不再退出，最终表现为 SEGV 或挂起。recover 模式（`halt_on_error=0`）报告后继续运行，此时内存已被破坏。
- 原生 ASan 在第一次越界访问时报告；MAIR 在出口报告，报告的“访问”是合并后的区间。

度量（`-O2`，静态，`-mair-stats` 与 E 组 `LoopReasons` 插件统计插桩后仍在循环内的检查）：

| | 7zip ASan | 7zip TSan | sqlite3 ASan（fuzzer-no-link） | sqlite3 ASan（无 fuzzer） |
|---|---|---|---|---|
| `lpoc` `exit-trip` | 212 | 191 | 92 | 133 |
| `lpoc` `range` + `periodic` | 1405 → 1813 | 1366 → 1740 | 649 → 763 | 681 → 838 |
| `lpoc` `rej-nocount` | 237 → 2 | 216 → 3 | 94 → 0 | 138 → 0 |
| 循环内残留检查 | 24843 → 24605 | 28295 → 28092 | 12619 → 12561 | 12256 → 12168 |
| 其中“仿射单 exiting block、无精确次数” | 499 → 155 | 460 → 149 | 113 → 2 | 164 → 10 |

`range` + `periodic` 增量里除 `exit-trip` 外的部分来自 `ptrtoint` 精确次数。残留总数降得比“单 exiting block”一类少，因为汇总后的检查放在内层循环出口，若外层循环有 barrier 或地址非仿射，它仍算外层循环里的残留，只是从每个内层迭代一次变成每个外层迭代一次。剩下的“单 exiting block”残留主要是一个 exiting block 跳到多个出口块（`switch`）的循环，以及外层循环里由内层汇总得到的符号大小区间。

TODO：
- 多出口（N4 第二步），包括一个 exiting block 用 `switch` 跳到多个出口块：需要在每条出口边放各自的区域，扩展物化。
- `cur + zext i32 {…}` 这类 32 位无符号下标（`wrap_idx`）：地址不是仿射递推，归为 `rej-nonaffine`，是第二步的前置。

测试：`test/MAIR/TestCases/loop-early-exit.ll`（正例 `until_neg`、判断之后的写 `store_after_test`、指针归纳变量、`copy_until`、32 位 `nsw` 计数器、无 wrap 标志的 `size_t` 计数器、`ptrtoint` 精确次数；负例条件执行、多出口、步长超过一页；统计行），`test/MAIR/TestCases/asan/mair-early-exit-oob.c`（越界读、越界写、仅指针归纳变量的越界写都报告，界内不误报，barrier 负例），`test/MAIR/TestCases/tsan/mair-early-exit-race.c` 与 `mair-early-exit-race-norace.c`（竞争仍报告；区间不多出停止元素之后、判断之后未写的元素，0 轮时为空），单元测试 `LoopPeriodicObservationCombinePass.ReadsTheTripCountOffAnExitValue`（按 BTC 求值的读写大小）、`ExitTripCountStopsAtACallBarrier`、`MAIRSchema.PtrToIntExpressionVerifiesAndPrints`。

## 地址合并

符号地址只有在能够证明字节差时才合并。有 ScalarEvolution 时使用地址 SCEV 的常量差；证明失败即保留原观察。没有该分析时，只用 LLVM 的常量偏移剥离匹配相同剩余地址，以 APInt 计算差值，不把整数 `add` 当成不回绕的地址距离。

两个 periodic 区域在基址、stride、trip count 都相同、access size 为常量、起点只差常量 `d`（不大于较低一方的 size）时合并成一个：每一轮里两段足迹相接或重叠，并集是一段 `[较低起点, max(a, d + b))`，所以结果保留较低起点、stride 和 trip count，只把 size 放大，正好是并集（`PeriodicRegion::merge`，`src/instrumentation/MAIR/MAIRRegion.cpp`）。起点按“根指针 + 常量 + 符号项”比较，偏移类型必须和指针索引同宽，常量差才精确。有空隙、stride 或 trip count 不同都不合并。合并后若 stride 不大于 size，区域变成连续的，TSan 用一次 range 访问记录，而不是逐轮调用 `__tsan_{read,write}_periodic`。测试：`test/MAIR/TestCases/locationmerge-periodic.ll`。

有空隙时还有一种精确合并：交错。m 个 periodic 区域基址、常量 access size `a`、trip count `T` 和常量 stride `S` 都相同，起点排序后等距 `g = |S| / m`（`m * g == |S|`），这是向量化/交错展开后常见的形状（例如 stride 32 的 +0 与 +16 两路 gather）。取 `s0` 为最低起点（`S < 0` 时为最高起点）、`d = S / m`，各起点为 `s0 + k * d`（`0 <= k < m`），第 k 个区域的第 i 轮访问 `s0 + k * d + i * S = s0 + (i * m + k) * d`，恰是起点 `s0`、stride `d`、access size `a`、trip count `m * T` 的区域的第 `i * m + k` 轮；反过来该区域的每一轮都唯一对应某个 (i, k)。所以并集正好是一个 periodic 区域，`g > a`（轮间有空隙）和 `g < a`（重叠）都精确，每一轮仍单独检查，空隙不查（`PeriodicRegion::mergeInterleaved`）。trip count 先零扩展 `ceil(log2 m)` 位再乘 m（`(2 * zext(T to i66))`），乘积不会回绕；常量 trip count 直接折叠。materialization 已支持 `Mul`/`ZeroExtend`，ASan/TSan 本来就把 trip count `ZExtOrTrunc` 到指针宽度，后端无需改动。

成员不能两两拼出：m = 3 时没有两条区域相距 `S / 2`，所以 LocationMerge 显式分组（`mergeInterleavedRange`，`src/instrumentation/MAIR/Transforms/LocationMergePass.cpp`）：periodic 类先按上段做两两相接合并，再对每个区域找候选（`PeriodicRegion::getInterleaveDistance`：基址、stride、size、trip count 相同，起点差为常量且 `0 < |d| < |S|`），从小到大尝试候选间距 g（m 尽量大），要求 `s0 + k * g` 全部在场；组由列表中最靠前的成员吸收其余成员，来源按列表顺序追加。合并后的区域 stride 变小，会再试一次。间距不整除 stride、`m * g != |S|`、size 或 trip count 不同都不合并。测试：`test/MAIR/TestCases/locationmerge-interleaved.ll`，运行时 `test/MAIR/TestCases/asan/mair-interleaved-runtime.c`（越界仍报告、界内与只 poison 空隙时不报告）。

Observation 合并保留完整来源指令序列及重复项。若右项严格包含左项，右项来源放在前面；相等、左项包含或非包含的区间并集仍按左后右追加。首项及首个有效 debug location 依赖这个顺序，不能把来源改成集合。

## ASan 的 lowering

ASan 按 Observation 的区域检查，而不是按来源指令逐个检查（`XsanAddressSanitizer::instrumentMAIRObservation` 和 `instrumentFunctionViaMAIR` 里的 `CanInstrumentViaMAIR`，`src/instrumentation/llvm23/AddressSanitizer.cpp`）。接受条件：每个来源都是 native ASan 也会检查的 load/store（`getInterestingMemoryOperands` 恰好给出一个操作数、不被安全栈/全局访问优化省掉、不是 scalable 向量）；区域是大小已知的连续区间（偏移可以是符号，大小可以是 trip count 表达式），或 periodic 区域。任一来源不满足时整个 Observation 退回 native，由 native 自己决定每个来源查不查。

native ASan 永远不查的 load/store（`getInterestingMemoryOperands` 为空：读写开关关闭、promotable 或 stack-safety 证明安全的 alloca 等；或被安全栈/全局访问优化省掉）在优化**之前**就从图中删掉（`MAOpt::eraseObservationsIf`，`src/instrumentation/MAIR/MAOpt.h`）。否则它们会与要查的访问合并，使整个合并 Observation 退回 native；在 7zip/re2/sqlite3 上，退回的多来源 Observation 全部含这样的来源（多数只由它们组成，另有 4/72/20 个拖回了要查的访问）。删除是可靠的：Observation 只会覆盖或合并别的 Observation，删掉它不会去掉其他访问的检查（反而避免了它覆盖一个要查的访问、自己退回 native 不查、把那次检查一起带走）；它的来源不再被建模，交给 native，native 照旧跳过。`isSafeAccess` 只取决于地址和访问大小，与检查放在哪无关。之后 `CanInstrumentViaMAIR` 不必再逐来源问 native。测试：`test/MAIR/TestCases/asan/mair-unchecked-sources.ll`（IR 与 select 统计）、`mair-unchecked-sources-oob.c`（越界仍报告）。

memintrinsic 是 call-like Observation，仍按 native 方式 lowering（`__asan_memcpy` 等），但它能覆盖后面的访问。native 不插桩的 memintrinsic 若留在图里，会替后面的访问“担保”却什么也不查，所以也在优化前删掉。ASan 没有关闭 memintrinsic 插桩的开关，但 `-asan-max-ins-per-bb`（默认 10000）让 native 在一个块里收集够这么多访问和 memintrinsic 后不再插桩，之后的 memintrinsic 不查。MAIR 按块统计 native 的候选（每条指令的 `getInterestingMemoryOperands` 个数加 memintrinsic，不做 native 的同址去重，所以是 native 计数的上界），计数达到上限之后的 memintrinsic 一律删掉（`MayBePastNativeCap`）；上限之前的 native 一定插桩。element-wise atomic 的 memintrinsic native 两个 sanitizer 都不插桩，但它们是原子 Observation，不覆盖别的访问，不必删。测试：`test/MAIR/TestCases/asan/mair-memintrinsic-cap.c`。

- 合并区间没有空洞：`AbstractAccess::merge` 只合并重叠或相邻的区间，`mergeExpressionSizedRanges` 要求区间长度不小于起点差，LIOM/COM/LPOC 只合并相等的区域，periodic 区域只在每轮足迹相接或重叠时把 size 放大，交错合并不放大 size 只缩小 stride（见“地址合并”），外层再汇总只在各次迭代的足迹首尾相接时合成区间（见“外层循环再汇总”）。所以查整个区间不会查到来源没碰过的字节。
- 区间起点的对齐由 MAIR 在建图和合并时维护，ASan 直接读取（`AbstractAccess::getAlign`，见 `docs/mair-region-alignment.md`）。例如 `struct { long a; int b, c; }` 的三个字段合并后，起点仍按 8 字节对齐。
- 单个未合并的访问（含固定宽度向量）照旧按 native 方式检查。不超过 16 字节的区间走内联检查：对齐时一次 shadow load，否则查首末两个字节，与 native 对不对齐访问的做法相同；中间至多跨一个 granule，ASan 自己的 redzone 不会这么窄。更长的区间或长度是表达式的区间调用 `__asan_loadN/storeN`，逐字节检查（`__asan_region_is_poisoned`）。
- 常量长度在 16 字节以上、不超过 `-as-mair-max-inline-range-size`（默认 128）字节的区间，先走内联快速路径（`XsanAddressSanitizer::insertMAIRRangeFastPath`），失败才调用 `__asan_loadN/storeN`，由它找出第一个 poison 字节并报告，报告内容（错误类型、READ/WRITE、合并后的大小、调试位置、地址）与只调用时完全一致。快速路径读区间碰到的**每一个** granule 的 shadow，不只查首尾，所以是精确的：当且仅当区间内有字节不可访问时失败。最后一个字节所在 granule 之前的 granule 必须整块可访问（shadow 为 0），按至多 8 字节一次读出，最后一次与前一次重叠（24B 对齐：两次 2 字节读；32B：一次 4 字节；40B：两次 4 字节；64B：一次 8 字节；128B：两次 8 字节），各次读出的值或在一起与 0 比较；最后一个 granule 按 ASan 的部分 granule 规则：shadow 为 0，或大于最后一个字节在 granule 内的偏移。起点对齐（region 的 `getAlign()`，见上上一条）不小于 granule 时 shadow 偏移是常量，长度是 granule 整数倍时不单独查最后一个 granule；否则 shadow 偏移由地址算出：起点到最后一个 granule 之间有 `(Size-1)/8` 或再多一个 granule，从起点按块读、再读一块紧挨最后一个 granule 结束的，覆盖两种情况；不需要先判断对齐的运行时分支。callback 模式（函数检查数超过 `-as-instrumentation-with-call-threshold`）、长度超过上限或为表达式、AMDGPU 仍只调用 runtime。上限由微基准确定：24–96B 内联比只调用快 4–17 倍，也不慢于 native 的逐字段内联检查；128–256B 仍更快但读的数据被访存噪声淹没；超过 128B 读 shadow 的次数随长度线性增加，默认取 128。测试：`test/MAIR/TestCases/asan/mair-inline-range.c`（IR：24/32/40/64/20B 对齐与不对齐、callback 模式、超过上限）、`mair-inline-range-runtime.c`（两端各越界一个字节的读写、中间 granule 整块或部分被 poison、界内与奇数起点不误报）。
- periodic 区域调用 `__asan_{load,store}_periodic(start, stride, size, count)`（recover 模式带 `_noabort`，`src/runtime/lib/asan/asan_rtl.cpp`）。runtime 按 stride 符号算出最低地址，count 为 0 不查；先用一次 shadow 扫描查整个外包区间，干净则全部合法；否则（或 stride 超过 1 KiB 时）按迭代顺序逐个访问检查，报告第一个越界访问的地址和单次访问大小，空隙中的 poison 不报。内核 ASan 和 `-asan-force-experiment` 没有这个入口，periodic 区域退回 native。
- 报告：读写种类（READ/WRITE，periodic 也一样选 `__asan_load_periodic` 或 `__asan_store_periodic`）和调试位置都取最先执行的来源，与 native 先查到的那次访问一致；大小可能是合并后的区间大小。来源的先后按指令支配关系判断（同一块内即指令顺序），在物化前用 MAIR 图的 DT 算好（`getFirstSource`，`src/instrumentation/llvm23/AddressSanitizer.cpp`），因为物化和先发出的检查会改 CFG 而不更新 DT。来源列表的顺序是合并顺序（LocationMerge 按偏移排序），不是执行顺序，不能直接取第一个。来自不同路径（如分支两侧经 COM 合并）的来源可能没有一个支配其余全部；这时取一个不被其他来源严格支配的来源，只由来源列表顺序决定，结果确定，但不代表运行时实际先执行的那个。`x[i]++` 因此按读报告，`p[0] = p[1]` 合并后的区间也按读报告，位置在 load。测试：`test/MAIR/TestCases/asan/mair-report-first-source.c`。
- 检查放在 Observation 所在的 ProgramPoint：LPOC 的区间/periodic 观察在循环出口（嵌套循环可再被外层不变外提提到外层 preheader），即访问发生之后，这是 ASan 已有的规则（见上文“输入与优化规则”）。

测试：`test/MAIR/TestCases/asan/mair-merged-range.c`、`mair-loop-range.c`（IR），`mair-range-runtime.c`（越界仍报告、界内不误报，含只有空隙被 poison 的 strided 访问），`mair-periodic-runtime.c`（直接调用 runtime 入口的边界情况）。

## TSan 的 lowering

TSan 同样按 Observation 的区域记录访问（`XsanThreadSanitizer::instrumentMAIRObservation` 和 `sanitizeFunctionViaMAIR` 里的 `CanInstrumentViaMAIR`，`src/instrumentation/llvm23/ThreadSanitizer.cpp`）。native TSan 永远不记录的 load/store（委托给 XSan、swifterror、`shouldInstrumentReadWriteFromAddress` 为假、读常量数据、未被捕获的 alloca）与 ASan 一样在优化前删掉（`NativeTsanOmits`，测试 `test/MAIR/TestCases/tsan/mair-unrecorded-sources.ll`）。开 `-ts-instrument-memintrinsics=0` 时 native 不记录 memintrinsic，它的 Observation 也在优化前删掉，否则它会覆盖后面的访问而自己不记录（测试 `test/MAIR/TestCases/tsan/mair-memintrinsics-disabled.c`）。vtable 指针访问也在优化前删掉，交回 native 的 `__tsan_vptr_{read,update}`：它们给访问打上 vptr 标记，竞态才报成 "data race on vptr"，range 与普通入口都表达不了这一点，所以 vptr 访问不参与 MAIR 优化，既不覆盖也不合并普通访问。剩下的每个来源还要是 load/store 且不是 TSan atomic。固定长度向量与 native 一样按整个存储大小算一次访问，可以与标量访问合并；scalable 向量的大小不是常量，区域检查拒绝它（native 也不记录）。测试 `test/MAIR/TestCases/tsan/mair-vector-vptr.ll`、`mair-race-vptr.cpp`。开 `-ts-distinguish-volatile` 时含 volatile 来源的 Observation 退回 native。TSan 的读、写、读写层次互不相同，合并只发生在同层次之间，所以不会把读记成写。

- 连续区间：2 的幂且不超过 16 字节时，与 native 单次访问相同地选择 `__tsan_{read,write}N`、`unaligned` 或 compound 变体（起点对齐取自 MAIR 区域，见 `docs/mair-region-alignment.md`）；其他大小（如 12、32 字节的向量，native 不记录）或运行时大小用 `__tsan_{read,write}_range`。LLVM 的 load/store 访问的是类型的 store size，总是整字节（i129 存 17 字节），所以不存在“非整字节”的访问。
- periodic 区域：常量 stride 不大于 access size 时足迹连续，按 `[Start, Start + (Trips - 1) * |Stride| + AccessSize)` 记一次 range（下降 stride 起点下移，trip 为 0 时 size 为 0，runtime 直接返回）。其他情况（有空隙或 stride 只在运行时已知）调用 `__tsan_{read,write}_periodic(start, stride, size, count)`（`src/runtime/lib/tsan/rtl/tsan_interface.inc`），runtime 逐轮记录 `size` 字节，空隙不记。`MemoryAccessPeriodicT`（新增在 `src/runtime/lib/tsan/rtl/tsan_rtl_access.cpp`，没有改动 TSan 原有代码）对每一轮的处理与 native 插桩在一次迭代里的做法相同，直接复用 TSan 原有的函数：
  - 1/2/4/8 字节、落在一个 shadow cell 内：`MemoryAccess`；
  - 1/2/4/8 字节、跨入下一个 cell：`UnalignedMemoryAccess`；
  - 16 字节：起点按 8 对齐时用 `MemoryAccess16`，否则拆成两次 `UnalignedMemoryAccess`；
  - 其他大小：`MemoryAccessRangeT`。

  这些函数内联进循环，各 cell 用的仍是 SSE 版本的 `ContainsSameAccess`/`CheckRaces`。某一轮的 shadow 已含本次访问时就不写 trace；竞态报在调用点；整次调用只取一次线程状态。

  Release runtime microbenchmark（stride 16、size 4）：
  - 同一 epoch 内重复访问，64 轮从 95.6 降到 50.9 ns，与 native 每轮一次 `__tsan_read4`（51.4 ns）持平。原实现每轮都完整走一次 range 入口并无条件写 trace。
  - 每次换 epoch 时与改前持平。

  测试：`test/MAIR/TestCases/tsan/mair-periodic-runtime.c`。
- 报告位置取最先执行的来源（`mair::getFirstSource`，`src/instrumentation/MAIR/Materialization/MAIRMaterialization.cpp`，与 ASan 共用）；访问类型就是 Observation 的类型。循环区间在出口处记录，所以与之竞争的另一线程访问常表现为 “Write … Previous read”。

测试：`test/MAIR/TestCases/tsan/mair-merged-access.c`、`mair-loop-range.c`、`mair-strided-loop.c`、`mair-report-first-source.c`（IR），`mair-race-*.c`、`mair-strided-loop.c`（竞争仍报告、空隙与界外不误报）。

## 可复现验证

- `test/MAIR/TestCases/loop-hierarchy.ll`：循环前/循环内检查隔离、循环内冗余、嵌套图、多个出口及无出口。
- `test/MAIR/TestCases/loop-normalization.ll`：缺少 preheader 和共享出口的输入规范化。
- `test/MAIR/TestCases/loop-irreducible.ll`：不可归约环不中断构图、不建循环层；环内地址不变的访问照常删除，地址随环变化的访问不被绕环的后支配者覆盖。
- `test/MAIR/TestCases/loop-invariant-observation-motion.ll` 与 `loop-range-aggregation.ll`：合法循环专用导出及重复执行稳定性。`repeated_row32` 同时检查内层周期合并得到的 128 字节读，以及随后不变外提把它提到外层 preheader。
- `test/MAIR/TestCases/loop-nested-symbolic.ll` 与 `loop-matmul.ll`：外层循环再汇总长度为表达式的区间和 periodic 区域，以及不满足首尾相接条件时保留内层结果。
- `test/MAIR/TestCases/mair-stats.ll`：`-mair-stats` 把第二次不变外提和内层区间合并分开打印。
- `src/instrumentation/MAIR/unittests`：图边界、方向 barrier、整树生命周期和 guard 物化回归。其中 `KeepsAccessCoveredOnlyInALaterIteration`、`EliminatesInvariantAccessCoveredInALaterIteration`、`IrreducibleCyclesAreOptimizedWithinOneIteration`、`CountsBarriersOfAnEndpointThatRunsAgain`、`DoesNotHoistAnAccessTheLoopCanExitWithout` 对应上面几条规则。
- `src/instrumentation/MAIR/unittests/RandomizedSoundnessTest.cpp`：随机生成带嵌套循环和不可归约环的函数，随机执行，要求每次写都有同地址、中间无 barrier 的 Observation，且每个 Observation 都有这样的写。
- `test/MAIR/TestCases/asan/mair-nonterminating-loop.ll`：终止假设下，ASan 的检查只留在后支配者处。
- `test/MAIR/TestCases/asan/mair-may-not-return.c`：ASan 的检查不会落到可能不返回的调用之后。`mair-nonterminating-loop.ll` 同时说明 PostDom 覆盖对 ASan 生效。
- `test/MAIR/TestCases/asan/mair-atomic.cpp`：两个互相覆盖的原子读只留第一个的检查。
