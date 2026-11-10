# MAIR 范围长度

连续范围可以用常量、LLVM 整数值或图拥有的整数表达式表示字节长度。表达式本身合法并不意味着它可以作为长度，例如 pointer add 的结果仍是指针。

连续范围的 SizeExpr 根类型必须为整数。Verifier 检查这个约束，instantiate 在修改 LLVM IR 或调用 backend 前验证整图；类型错误会直接拒绝物化，并保留原图和 IR。

回归测试：`MAIRMaterialization.RejectsPointerContiguousSizeBeforeMutation`。完整检查在 build 中运行 `ninja check-mair`。

## 长度运算与溢出

已有整数表达式按各节点的原始位宽执行模算术。合并两个相同长度、不同起点的连续范围时，必须证明长度的无符号下界不小于起点距离；不能把原表达式当作无限精度整数。例如 i9 的 `5 * (1 + zext(i8 n))` 会回绕，不能据此认定长度至少为 5。

MAIR 使用 LLVM `ConstantRange` 的常量、扩展、截断、加法和乘法传递函数分析已有表达式；其他整数节点按完整范围处理。判断使用 `getUnsignedMin()` 与 `getUnsignedMax()`，不把循环区间端点当作普通上下界，也不将最高位为 1 的长度误当负数。无法证明相邻或重叠时不合并。

新增的长度运算与原表达式不同：它们必须能表示数学结果，不能引入回绕。

- 合并范围的 `size + delta`：先用 APInt 计算最大可能和；必要时整体零扩展原 SizeExpr，再做加法。不会穿过原表达式的内部运算改变其语义。
- 循环范围的 `count * access_size`：用 ScalarEvolution 的 `getUnsignedRangeMax()` 计算乘积所需位宽；先零扩展执行次数，再通过 `getMulExpr()` 构造乘法，并对最终 SCEV 做转换预检。
- 位宽按结果上界确定，不固定到 i64 或指针宽度；无法表示为合法 LLVM 整数类型时拒绝该范围构造。零次执行保持零长度。

例如 i9 的 `200 + zext(i8 n)` 在 n=200 时长度为 400，起点相差 200 的两段合并为 i10 的 600；i8 计数器执行 256 次、每次访问 4 字节时，长度必须为 1024。

回归测试在 `MAIRMaterializationTest.cpp` 中先优化、物化，再替换输入并使用 LLVM 常量折叠验证 backend 收到的实际 AccessSize，覆盖零次、窄计数器全周期、正反合并顺序及原 Add/Mul 回绕。`loop-nested-range.ll` 同时检查嵌套循环聚合与重复运行稳定性。验证范围是 MAIR 图、物化和 LLVM IR 合法性，不代表所有 sanitizer runtime 已支持动态范围。
