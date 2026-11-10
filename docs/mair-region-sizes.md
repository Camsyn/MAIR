# MAIR 范围长度

连续范围可以用常量、LLVM 整数值或图拥有的整数表达式表示字节长度。表达式本身合法并不意味着它可以作为长度，例如 pointer add 的结果仍是指针。

连续范围的 SizeExpr 根类型必须为整数。Verifier 检查这个约束，instantiate 在修改 LLVM IR 或调用 backend 前验证整图；类型错误会直接拒绝物化，并保留原图和 IR。

回归测试：`MAIRMaterialization.RejectsPointerContiguousSizeBeforeMutation`。完整检查在 build 中运行 `ninja check-mair`。
