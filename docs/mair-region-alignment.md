# MAIR 区域的对齐

区域的对齐是 MAIR 自己维护的通用信息，不是某个 sanitizer 的推导。它的含义是"区域第一个字节的地址已知按 N 字节对齐"。MAIR 在建图时记录它，在每次合并时维护它；sanitizer 只读取（`AbstractAccess::getAlign`、`PeriodicRegion::getAlign`，声明在 `src/instrumentation/MAIR/MAIR.h`）。

对齐描述的是地址的事实，不影响区域访问了哪些字节，所以 `operator==` 不比较它，两个只差对齐的区域仍然相等。dump 中只有 N > 1 时才在末尾打印 `, align N`；没有这一项表示只知道按 1 字节对齐。

## 来源

`Observation::fromInstruction`（`MAIRObservation.cpp`）和 memtransfer 的建图（`MAIR.cpp`）从指令本身取得对齐：

| 指令 | 对齐 |
| --- | --- |
| load、store、atomicrmw、cmpxchg | 指令声明的 `align` |
| memset | 目的指针的对齐 |
| memcpy、memmove | 目的、源各自的对齐 |
| masked load/store | 指针参数的 `align` |

## 合并

合并后的区间（`AbstractAccess::merge`，`MAIRRegion.cpp`）从最左端那部分的起点开始，因此至少继承最左端那部分的对齐。另一部分如果从起点之后 d 字节处开始、对齐为 A，就能证明起点按 `commonAlignment(A, d)` 对齐。合并取两者中较强的一个。

例如，偏移 0 处有一个 `i64 align 4`，偏移 8 处有一个 `i64 align 16`。后者证明起点按 gcd(16, 8) = 8 对齐，比最左端自己的 4 更强。

各种合并形态的处理：

| 合并形态 | 结果的对齐 |
| --- | --- |
| 相等 | 两边取较强 |
| 包含 | 容器继承被包含那部分在其距离处证明的对齐 |
| 常量偏移 | 用两段到新起点的距离计算 |
| 不可比的符号偏移 | 用指针差作为距离 |
| 长度为表达式 | 用常量偏移差作为距离 |
| masked 区域 | 边界相同，两边取较强 |

### 归纳不变式

每个区域的对齐都不弱于它的任一来源在其距离处能证明的对齐。

证明：两个整数都被 2^k 整除时，它们的和也被 2^k 整除，因此 `commonAlignment(commonAlignment(A, x), y) <= commonAlignment(A, x + y)`。逐次合并的结果因此不弱于一次性对所有来源计算的结果。

这条不变式也保证，原先 ASan、TSan 各自的推导能得到的对齐，现在都包含在 MAIR 的值里：
- ASan 原先取"来源对齐的最小值"，再结合常量偏移推出基址对齐；
- TSan 原先只用"能算出距离的来源"推导。

## 循环

`PeriodicRegion` 的对齐指每一趟（`Start + i * Stride`）起点的对齐。

**LPOC 生成区域时：** 循环里的访问每次执行都满足它声明的对齐。因此不论是 periodic 区域，还是覆盖所有趟的连续区间，都继承循环体内那个区域的对齐。区间向上时从第一趟开始，向下时从最后一趟开始，起点都是某一趟的起点。

**合并 periodic 区域时：**
- 相邻合并（`PeriodicRegion::merge`）：每一趟从低处那个区域的那一趟开始，结果的对齐是 `max(低处的对齐, commonAlignment(高处的对齐, 距离))`。
- 交错合并（`mergeInterleaved`）：并集的每一趟都是某个切片的某一趟，结果的对齐取各切片对齐中的最弱者。

## 使用方

- TSan（`instrumentMAIRObservation`，`src/instrumentation/llvm23/ThreadSanitizer.cpp`）：对 2 的幂且不超过 16 字节的区间，按起点对齐选择 `__tsan_*` 或 `__tsan_unaligned_*`。
- ASan（`instrumentMAIRObservation`，`src/instrumentation/llvm23/AddressSanitizer.cpp`）：把起点对齐交给 `doInstrumentAddress`，决定走一次 shadow load 的快路径，还是检查首尾两个字节。

## 测试

- `test/MAIR/TestCases/alignment.ll`：
  - 最左端对齐、后面的部分证明更强的对齐、起点在基址之后、未知对齐、masked；
  - LPOC 的上行区间与下行区间、periodic 相邻合并、交错合并。
- 单元测试 `MAIRGraph.AbstractAccessMergeKeepsStrongestStartAlignment`（`src/instrumentation/MAIR/unittests/MAIRGraphTest.cpp`）：合并顺序、包含、相等。
