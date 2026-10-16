# 值对象字面量部分初始化清理修复

> **状态：从 union 布局优化中拆出，修复和回归用例保存为独立 patch，当前未应用。日期：2026-10-16。**
>
> 补丁：[feng-value-literal-partial-cleanup-bugfix.patch](feng-value-literal-partial-cleanup-bugfix.patch)。
> 应用基线：第一阶段提交 `65672f13` 加本轮纯 union 布局变更。

## 1. 发现过程与根因

补充 union 异常生命周期覆盖时，构造 `@value Pair`：第一个字段取得托管资源，
第二个字段求值抛异常。第一个字段的终结器没有执行。在 `65672f13` 的旧 union
布局下也已复现，因此不是移除 `_fwd` 引入的问题。

`cg_emit_object_literal` 的值对象路径在逐个初始化显式字段时，没有把正在构造的
完整值登记到 cleanup 链。后续字段求值失败，已经写入的资源没有清理责任方。

## 2. 修复边界

在显式字段初始化期间，复用 tuple 构造已使用的拥有存储保护机制，登记完整的
aggregate 或具化存储。异常时清理部分构造的值；成功时仅撤销临时保护并转移
原有拥有权，不额外增加 retain／release 对。

实现只修改 `src/codegen/codegen.c` 的值对象字面量构造路径。不改变 union 布局、
循环类型识别、异常协议、runtime API 或 DAP。清理保护有必要的登记成本，不能
将“不新增 retain／release 对”解释为没有任何运行时成本。

## 3. 独立回归覆盖

补丁包含独立 FCTS 文件及入口注册，保留拆分前的三个失败场景和断言：

- 普通值对象后续字段抛异常：检查已写入资源和已完成 union 的清理次数、顺序。
- 值对象字段包含 union，活动成员又是多资源 aggregate：检查嵌套清理顺序。
- 源码隐藏的泛型 provider 构造部分值：检查参数拥有权和调用方临时值各自清理。

正常 union 构造、完整 union 异常清理仍留在布局优化用例中。上述缺陷用例随补丁
迁移，不能通过跳过断言或改变预期掩盖未应用修复时的问题。

## 4. 应用与验证

从仓库根目录执行：

```sh
git apply --check docs/engineering/feng-value-literal-partial-cleanup-bugfix.patch
git apply docs/engineering/feng-value-literal-partial-cleanup-bugfix.patch
```

补丁以纯 union 版本为基线，不依赖另外两份 bugfix patch。已与其他修复共同通过的
历史全量回归不能代替拆分后的独立验证；应用后仍需在允许的环境执行 `make test`。

- [x] 修改前编译器复现资源遗漏；组合实现通过 Linux sanitizer 和普通全量回归。
- [x] 核验独立补丁应用及反向还原；三份补丁的六种应用顺序均通过，组合后的实现与拆分前备份逐字节一致。
- [x] Linux ARM64 隔离目录仅应用本补丁，普通 FCTS 1684/1684 通过，包含三个迁移用例。
- [ ] 人工决定应用后，执行全量回归并更新本状态。

拆分验证证据：`temp/union-layout-split-20261016/partial-fcts.log` 和
`partial-fcts.status`（退出码 0）。这不是本补丁应用后的全量 `make test`；
主工作区仍未应用本补丁，后续正式应用时按上述计划回归。
