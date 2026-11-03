# Feng 值类型接收者 `self` 的闭包捕获约束优化方案

> **状态**：目标规则已确定，待实施。更新日期：2026-11-03。
>
> 本文是独立开发方案。本轮仅记录方案，不修改正式规范、编译器、runtime 或测试；可与 async/await 分别交付。

## 1. 现状与问题

现行 [函数规范](../specifications/feng-function.md)允许 Lambda 捕获值类型 `self`，按值复制；
[类型规范](../specifications/feng-type.md)同时规定普通实例方法直接操作原接收者存储。

以下行为已在 `@value type` 实例方法中复现。

- 方法体存在捕获 `self` 的 Lambda 时，Codegen 在方法入口创建一个托管捕获对象，将值类型接收者复制进去。
- 同一次方法调用中的多个 Lambda 共享这份捕获存储；闭包外的 `self` 也被改绑到该存储。
- 因此，仅加入捕获 `self` 的 Lambda，就会改变外层方法的字段修改目标，不以闭包是否被调用为条件。
- 捕获对象由各闭包独立强持有；释放一个闭包后，其他闭包仍可使用。已验证的偏差是接收者语义，不是提前释放。

复现：

```feng
spec Reader(): int;

@value
type Counter {
    var value: int;

    func makeReader(): Reader {
        self.value += 1;
        return () -> self.value;
    }
}
```

原变量初值为 `1` 时，当前调用 `makeReader()` 后原变量仍为 `1`，返回的闭包读到 `2`。
外层 `self.value += 1` 没有修改原接收者，与普通实例方法语义不一致。

实现定位：`src/codegen/codegen.c` 中的 `cg_bind_concrete_callable_self`、
`cg_scope_bind_capture_cell` 和 `cg_emit_lambda_closure_type`。

内建标量（整数、浮点、布尔）和枚举的 `fit` 实例方法已拒绝 `() -> self`，诊断为 `AE0003`。在方法体先写 `let it = self;` 再返回 `() -> it`，闭包可以读取复制时的值。本次沿用标量和枚举的现有实现与诊断。[内建类型的 fit](../specifications/feng-fit-builtin-type.md) · [枚举规范](../specifications/feng-enum.md)

## 2. 目标规则

**对所有能作为实例接收者的值类型，源码 Lambda 均禁止直接捕获外层 `self`。需要快照时，在 Lambda 外显式复制到局部绑定。**

C# 的元组语法不能声明实例方法。Feng 的具名元组通过 `fit` 定义实例方法，其 `self` 捕获与 `@value type` 一样采用 C# 的 `struct` 捕获规则。[C# 元组](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/builtin-types/value-tuples) · [C# 匿名函数规则 §12.22.3](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#12223-anonymous-function-bodies)

1. Lambda 读取上述接收者、通过它修改字段或调用成员，均在编译期报错。嵌套 Lambda 同样检查，无论它是否逃逸或被调用。
2. 本次只对 `@value type` 和具名元组新增检查。`@value type` 覆盖 `type` / `fit` 实例方法、构造函数和字段初始化 Lambda。具名元组覆盖 `fit` 实例方法。两者均包含泛型形式。
3. 在闭包外执行 `let it = self;` 或 `var it = self;`，按普通值赋值规则复制一次。随后捕获 `it` 使用现有局部绑定捕获规则；多个闭包捕获同一 `it` 时共享该绑定，不分别复制。
4. 在 Lambda 内执行 `let it = self;` 仍然捕获了外层 `self`，必须报错。本次新增的诊断应提示在闭包外建立局部绑定。
5. 普通局部绑定、引用类型 `self` 的捕获，以及 `obj.method` 方法值的既有规则保持不变。

允许的替代写法：

```feng
spec Reader(): int;

@value
type Counter {
    var value: int;

    func makeReader(): Reader {
        let it = self;              // 此处复制当前值
        self.value += 1;            // 修改原接收者
        return () -> it.value;      // 捕获普通局部绑定
    }
}
```

原变量初值为 `1` 时，这段代码应使原变量变为 `2`，闭包读到 `1`。
`let` 仅限制 `it` 的重新绑定；其字段可变性、值内托管引用的复制和释放均沿用普通规则。

在具名元组的 `fit` 方法中，`() -> self.item1` 即使只读也报错。先写 `let it = self;`，再用 `() -> it.item1` 即可。

## 3. 实施边界

- 先在[函数规范](../specifications/feng-function.md)中定义 Lambda 捕获 `self` 的规则，[类型规范](../specifications/feng-type.md)和[元组规范](../specifications/feng-tuple.md)中的相关说明改为引用。
- Semantic 在源码捕获分析中按接收者类型拒绝非法 `self` 捕获，复用既有捕获信息和类型身份；不能只在 Codegen 报错，也不能仅检查 `self.field` 的语法形式。
- 合法的 `let it = self` 继续走普通局部绑定捕获路径；外层同步方法的 `self` 始终访问原接收者存储。不得再为非法直接捕获建立值类型 `self` 的捕获副本。
- 复用现有捕获对象、ARC 和循环回收机制；不新增 runtime API，不改变普通局部绑定的捕获布局或共享方式。

## 4. 验收

| 层次 | 必须覆盖 |
|---|---|
| Semantic | 覆盖第 2 节所列上下文及其泛型形式。Lambda 读取 `self`、通过它修改字段或调用成员、在 Lambda 内复制 `self` 均报错。包含嵌套捕获及元组只读 `self.item1` 的编译失败用例 |
| Codegen | 合法显式复制只为被捕获的 `it` 建立捕获存储；外层 `self` 保持原接收者绑定 |
| FCTS | 显式复制前后原接收者与快照独立；多个闭包共享同一 `it`；释放其中一个不影响其他闭包，最终持有释放后资源恰好清理一次 |
| 回归 | 标量和枚举的 `fit` 保留现有捕获行为与诊断。普通局部绑定、引用类型 `self` 和值类型方法值继续按既有规则验收。实现交付运行完整 `make test` |

[现有元组 FCTS](../../fcts/fcts_bin/src/test_tuple.ff) 的 `tget_sum_lambda` 直接捕获了 `self`。实施时经授权改为捕获显式副本 `it`，保留直接调用和经 `spec` 调用的两组结果断言。

## 5. 与 async/await 的关系

本项独立实施，不作为 async/await 第一阶段的前置修复。
编译器将 async 方法的接收者输入保存在状态机字段中，不属于源码 Lambda 捕获；
异步接收者语义由 [async/await 草案 §4.1](./feng-async-future-draft-4.md#41-创建与首次执行)单独定义，本文不重复定稿。

若 async 方法内部另有源码 Lambda，仍在 lowering 前按源码接收者类型检查上述捕获限制。
不得因为后续会生成状态机，就把非法的值类型 `self` 捕获变成合法捕获。
