# Feng 类型静态聚合绑定代码生成修复

> 状态：已完成（completed）

## 1 问题

类型静态绑定在解析为 C 聚合类型时，当前代码生成仍沿用标量声明和初始化路径。
例如：

```feng
spec Target {
  let id: i32;
}

type Holder<T> {
  static var current: T;

  func set(value: T) {
    Holder<T>.current = value;
  }
}

let value = Holder<Target>.current;
```

`Holder<Target>.current` 的闭合字段类型是 object-form spec fat value，但生成的 C
声明使用：

```c
struct FengSpecValue__Target holder_target_current = 0;
```

C 不允许使用整数 `0` 初始化结构体，因此 host C 编译失败。修正声明后还能观察到：
闭合实例已经分别生成静态存储，但泛型共享方法体仍读写 `Holder<T>` 的开放占位存储，
没有根据当前闭合类型描述符选择 `Holder<Target>.current`。该问题属于类型静态绑定的
值类别分派和泛型具体化信息缺失，不是泛型或 object-form spec 的语言限制。

## 2 根因与范围

模块级绑定已经按统一值类别生成静态存储：

- managed pointer：`= NULL`；
- aggregate 和其他 by-value struct：`= {0}`；
- scalar：`= 0`。

类型静态绑定存在两个同源缺口：

1. 静态存储声明只区分 managed pointer 与其他类型，错误地将 aggregate 和
   by-value struct 归入 scalar。其显式初始化路径也缺少 aggregate 赋值和普通
   by-value struct 直接赋值分支；静态 `var` 的后续写入已有 aggregate 分支，但仍
   缺少普通 by-value struct 分支。
2. 泛型类型会为开放实例与每个闭合实例分别注册静态存储。泛型共享方法体当前直接引用
   开放实例符号；闭合 wrapper 虽然传入了具体 `_type_desc`，描述符中却没有静态绑定
   状态，因此共享方法体无法选择当前闭合实例的静态存储及初始化标记。

本次修复覆盖所有类型静态绑定，不为泛型、object-form spec 或 TUI 增加特判：

1. 非泛型与泛型闭合类型；
2. object-form spec 等带 managed slots 的 aggregate；
3. 不带 managed slots 的 `@value` / tuple 等 by-value struct；
4. 无显式初始化、带显式初始化和静态 `var` 后续写入。

不改变泛型实例化规则、静态成员可见性或 object-form spec ABI。为使现有闭合类型描述符
能够携带静态状态，本次扩展 runtime 私有的 `FengTypeDescriptor` 和
`FengAggregateDescriptor`；不新增 runtime 函数 API。

## 3 代码生成规则

### 3.1 静态存储声明

类型静态绑定与模块级绑定使用相同的值类别规则：

```text
managed pointer                    -> = NULL
aggregate / other by-value struct -> = {0}
scalar                             -> = 0
```

### 3.2 无显式初始化

- aggregate 继续通过对应 aggregate default-init 规则初始化；
- managed pointer、by-value struct 与 scalar 使用各自的默认值规则；
- 静态存储在 ensure-init 前已经零初始化，default-init 负责建立 Feng 语言要求的默认值。

### 3.3 显式初始化

- managed pointer：按现有 owns-ref 规则转移或 retain；
- aggregate：物化稳定源值后调用 `feng_aggregate_assign`；
- 其他 by-value struct：使用 C 结构体直接赋值；
- scalar：保留标量转换赋值。

### 3.4 静态 `var` 后续写入

- managed pointer：使用现有 ARC 赋值路径；
- aggregate：继续使用 `feng_aggregate_assign`；
- 其他 by-value struct：使用 C 结构体直接赋值；
- scalar：保留标量转换赋值。

### 3.5 泛型共享方法体

泛型类型的静态状态按闭合类型隔离。显式闭合访问继续直接引用对应静态符号；共享方法体
必须通过当前闭合类型描述符取得静态绑定，不得读写开放实例的占位存储，也不得执行运行时
名称查找或线性遍历。

每个闭合类型按声明顺序生成静态绑定状态 sidecar。每一项包含：

```text
storage       静态存储地址
initialized   该闭合静态字段的惰性初始化标记
```

每个泛型静态字段生成一个共享 ensure-init 函数。该函数接收当前闭合类型描述符和对应
sidecar 项，检查 `initialized` 后按字段声明执行初始化。共享方法体使用编译期确定的成员
索引取得 sidecar 项，并直接调用该字段的共享 ensure-init 函数；不通过函数指针调用，
也不提前初始化未实际访问的静态字段。静态绑定类型为泛型参数时，初始化、值复制与所有权
操作继续由当前类型参数描述符分派；拥有所有权的 aggregate 初始化通过
`feng_aggregate_take` 直接转移，不增加 retain/release。

显式闭合访问仍可直接引用对应静态存储；其 ensure-init 与共享访问必须使用同一 sidecar
初始化标记，保证两条访问路径只初始化一次且观察到同一状态。

闭合泛型的静态存储和 sidecar 使用与现有闭合泛型描述符相同的 weak 定义，使 provider 与
consumer 生成同一闭合实例时由链接器合并为唯一状态。sidecar 的声明顺序包含进入 FT 的
内部表示成员，因此 provider 内部的 seal 静态字段也保持相同索引。

把 sidecar 指针加入 `FengTypeDescriptor` 与
`FengAggregateDescriptor`。泛型 heap type 与泛型值类型方法已经分别接收这两个描述符，
因此不增加隐藏参数，也不改变非泛型方法签名。该调整属于 runtime 私有 ABI 变更，实施前
已经过人工确认。

## 4 性能与所有权

- 非泛型及显式闭合的 scalar 与 managed pointer 路径不增加运行时分支或间接调用；
- aggregate 只执行其正确所有权语义必需的 `feng_aggregate_assign`；
- by-value struct 继续使用直接 C 赋值；
- 泛型共享方法体每次访问静态绑定时增加一次固定索引 sidecar 读取，并通过 `storage`
  指针访问闭合静态存储；原有 ensure-init 保持一次直接调用，不增加函数指针间接调用；
- 描述符只保存 sidecar 指针，不保存运行时不使用的成员数量；
- 不执行名称查找、动态遍历、堆分配或提前初始化；
- 不新增 runtime 函数 API，但扩展两个私有描述符结构。

## 5 验证

- 新增自动发现的 smoke 用例，覆盖泛型静态 `T` 在闭合为 object-form spec 时的默认值、
  读取、写入、替换和清空，并验证不同闭合泛型实例的静态存储相互独立；
- 覆盖静态 aggregate 显式初始化；
- 覆盖普通 by-value struct 静态绑定的默认值、显式初始化和后续写入；
- 增加 fcts 语言行为用例并注册执行，覆盖跨包调用、provider 内部 seal 静态字段、
  heap/value owner、managed/aggregate 类型参数以及拥有所有权的 aggregate 初始化；
- 执行 `make test` 全量回归。

## 6 实施清单

- [x] 修复类型静态绑定的 C 静态存储声明
- [x] 修复类型静态绑定的显式初始化
- [x] 补齐类型静态 `var` 的 by-value struct 写入
- [x] 为闭合泛型类型生成静态绑定 sidecar
- [x] 让泛型共享方法体通过闭合描述符访问静态绑定
- [x] 新增 smoke 与 fcts 回归用例
- [x] 执行 `make test`
