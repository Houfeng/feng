# Feng `@value` 类型 `self` 闭包捕获约束优化方案

> **状态**：目标规则已确定，待实施。更新日期：2026-10-28。
>
> 本文是独立开发方案。本轮仅记录方案，不修改正式规范、编译器、runtime 或测试；可与 async/await 分别交付。

## 1. 现状与问题

现行 [函数规范](../specifications/feng-function.md)允许 Lambda 捕获值类型 `self`，按值复制；
[类型规范](../specifications/feng-type.md)同时规定普通实例方法直接操作原接收者存储。

当前实现及临时验证确认：

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

## 2. 目标规则

与 [C# 匿名函数规则 §12.22.3](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#12223-anonymous-function-bodies)对齐：
**禁止源码 Lambda 捕获外层 `@value type` 的 `self`；需要快照时，先显式复制到局部绑定。**

1. Lambda 对外层值类型 `self` 的读取、字段修改和成员调用均在编译期报错，嵌套 Lambda 同样检查；不按是否逃逸或是否被调用豁免。
2. 检查依据 `self` 所属接收者类型，覆盖 `@value` 类型及其泛型实例的 `type` / `fit` 实例方法、构造函数和字段初始化 Lambda。
3. 在闭包外执行 `let it = self;` 或 `var it = self;`，按普通值赋值规则复制一次。随后捕获 `it` 使用现有局部绑定捕获规则；多个闭包捕获同一 `it` 时共享该绑定，不分别复制。
4. 在 Lambda 内执行 `let it = self;` 仍然捕获了外层 `self`，必须报错。诊断提示应明确要求在闭包外建立局部绑定。
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

## 3. 实施边界

- 先修订 [函数规范](../specifications/feng-function.md)中的 Lambda `self` 捕获规则；类型规范及关联说明引用该规则，移除冲突描述。
- Semantic 在源码捕获分析中按接收者类型拒绝非法 `self` 捕获，复用既有捕获信息和类型身份；不能只在 Codegen 报错，也不能仅检查 `self.field` 的语法形式。
- 合法的 `let it = self` 继续走普通局部绑定捕获路径；外层同步方法的 `self` 始终访问原接收者存储。不得再为非法直接捕获建立值类型 `self` 的捕获副本。
- 复用现有捕获对象、ARC 和循环回收机制；不新增 runtime API，不改变普通局部绑定的捕获布局或共享方式。

## 4. 验收

| 层次 | 必须覆盖 |
|---|---|
| Semantic | 直接读取、修改、成员调用、嵌套 Lambda、Lambda 内复制 `self` 均拒绝；覆盖 `type` / `fit`、泛型 `@value` owner、构造和字段初始化上下文 |
| Codegen | 合法显式复制只为被捕获的 `it` 建立捕获存储；外层 `self` 保持原接收者绑定 |
| FCTS | 显式复制前后原接收者与快照独立；多个闭包共享同一 `it`；释放其中一个不影响其他闭包，最终持有释放后资源恰好清理一次 |
| 回归 | 普通局部绑定、引用类型 `self`、值类型方法值保持既有行为；实现交付运行完整 `make test` |

现有测试的调整须按仓库规则获得授权；本轮不增加或修改测试。

## 5. 与 async/await 的关系

本项独立实施，不作为 async/await 第一阶段的前置修复。
编译器将 async 方法的接收者输入保存在状态机字段中，不属于源码 Lambda 捕获；
异步接收者语义由 [async/await 草案 §4.1](./feng-async-future-draft-4.md#41-创建与首次执行)单独定义，本文不重复定稿。

若 async 方法内部另有源码 Lambda，仍在 lowering 前按源码接收者类型检查上述捕获限制。
不得因为后续会生成状态机，就把非法的值类型 `self` 捕获变成合法捕获。
