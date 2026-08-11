# Feng FT 嵌套泛型类型参数序列化修复

> 状态：实现与专项验证完成，等待全量回归

## 1 问题

导出成员类型 `Action<KeyEvent<T>>` 时，FT 写入器在递归序列化
`KeyEvent<T>` 之前就记录外层 Action 的 TSEQ 起点。内层序列化会先追加自己的
TSEQ 项，外层随后追加的参数 type-id 已不在原起点，因此导入端把外层参数错误读取为
内层的 `T`；闭合后表现为 `Action<Widget>`，丢失 `KeyEvent<...>` 包装。

## 2 修复规则

序列化 named generic 类型时必须：

1. 先递归序列化全部 type argument，并暂存各自 type-id；
2. 递归完成后，以当前 TSEQ 尾部确定外层连续区间起点；
3. 按参数顺序连续追加外层 TSEQ 项。

这样每个 named generic 记录引用的 TSEQ 区间始终连续，不受任意深度的内层泛型追加
影响。FT 格式本身不变，不升级版本，也不改变导入端和运行时 ABI。

## 3 验证

- 增加跨包 fcts 用例，通过 FT 导出、导入和闭合调用覆盖嵌套泛型参数结构；
- 通过跨包 TUI DEMO 验证 `Action<KeyEvent<Widget>>` 与
  `Action<MouseEvent<Widget>>` 保持完整；
- 执行 `make test` 全量回归。

## 4 实施清单

- [x] 修正 named generic 的 TSEQ 写入顺序
- [x] 增加跨包 FT round-trip 回归用例
- [x] 验证 TUI DEMO 构建
- [ ] 执行 `make test`
