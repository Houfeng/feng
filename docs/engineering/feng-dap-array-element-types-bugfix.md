# DAP 内联数组元素类型与步长修复

> **状态：从 union 布局优化中拆出，修复和回归用例保存为独立 patch，当前未应用。日期：2026-10-16。**
>
> 补丁：[feng-dap-array-element-types-bugfix.patch](feng-dap-array-element-types-bugfix.patch)。
> 应用基线：第一阶段提交 `65672f13` 加本轮纯 union 布局变更。

## 1. 发现过程与根因

新增 DAP 布局验证时，局部 union 和对象中的 union 字段显示正常，但 union 数组
元素显示为地址且不能展开。修改前编译器也已复现。

`proxy_array_element_c_type` 对无法识别的具名类型退回 `void *`，内联 union 被按
指针宽度计算元素间隔。数组元素的返回值也未完整复用既有载体展示处理。

## 2. 修复边界

编译器为具体用户类型、spec 及 enum 生成 Feng 类型到 C 表示的调试映射；开放泛型
占位载体不产生具体类型记录。DAP 根据记录使用真实 C 类型作数组下标访问，再复用
现有载体展示逻辑。普通值对象数组同样使用这一规则。

补丁涉及 `codegen.c`、`mapping.c/.h`、`debug.c`、`dap/proxy.c`，并携带
`feng-dap-dev.md` 的必要规范更新。`.fd` 增加 `type` 记录种类，复用现有 ENTS
记录宽度、section 和版本号；旧读取器不识别新种类，调试器及调试制品需要同步更新。

不改变 union 布局、runtime、循环引用识别、部分初始化清理或 LTO 选项。纯布局
版本继续验证局部和对象字段中的 union；数组缺陷用例随本补丁保存。

## 3. 独立回归覆盖

- 类型映射：生成、去重、非法记录拒绝、`.fd` 读写及依赖合并冲突拒绝。
- 真实 DAP：默认 Debug 和显式 ThinLTO，局部、字段、不同元素值及仅经数组使用
  的 union；嵌套、空、可写和默认值数组；普通值对象数组的正确步长和展开。

## 4. 应用与验证

```sh
git apply --check docs/engineering/feng-dap-array-element-types-bugfix.patch
git apply docs/engineering/feng-dap-array-element-types-bugfix.patch
```

补丁以纯 union 版本为基线，不依赖另外两份 bugfix patch。应用前主 DAP 规范保持
当前实现规则，不能提前将尚未应用的类型记录写为已交付能力。

- [x] 修改前编译器复现数组读取错误；组合实现通过 Linux sanitizer 和普通全量回归。
- [x] 核验独立补丁应用及反向还原；三份补丁的六种应用顺序均通过，组合后的实现与拆分前备份逐字节一致。
- [x] 在 Linux ARM64 隔离目录仅应用本补丁，类型映射、`.fd` 读写与依赖合并
      用例通过；真实 DAP 数组展示在默认 Debug 和显式 ThinLTO 下通过。
- [ ] 人工决定应用后，执行全量回归并更新本状态。

拆分后专项验证退出码为 0，记录为
`temp/union-layout-split-20261016/dap-focused.log` 和 `dap-focused.status`。
本次没有对单独应用本补丁的版本执行全量 `make test`。
