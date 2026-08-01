# Feng 运算符重载草案

> **状态：草案（Draft）**  
> **日期：2026-07-16**  
> 本文只记录运算符重载的候选设计，不是当前语言规范，不代表该能力已经确定实施，也不包含开发任务清单。

## 1 背景与目标

Feng 当前不支持用户定义的运算符重载。若未来增加该能力，设计需要继续遵循以下基础原则：

1. compiler 与 runtime 不感知任何 `std` 或其他上层库类型。
2. compiler 只提供用户层无法独立实现的语言原语，并保持 runtime API 最小化。
3. 运算符重载不引入新的运行时分派、元数据或 ABI surface。
4. 运算符不是独立的可调用实体，其实现本体始终是普通顶层函数。
5. `@operator` 只为普通函数增加一种编译期调用语法糖，函数仍可按原名称正常调用。
6. 运算符实现不依附于左操作数或右操作数类型，避免引入 receiver 方向和左右归属问题。

本文只讨论基于内建注解 `@operator("...")` 的候选方向。是否正式交付、支持哪些运算符以及具体诊断码，均需在实施前另行确定。

## 2 核心模型

### 2.1 运算符是普通顶层函数

运算符实现声明为普通顶层函数，并通过内建注解声明对应的运算符符号：

```feng
@operator("+")
open func vector_add(left: Vector, right: Vector): Vector {
  // ...
}
```

该函数仍可按普通函数调用：

```feng
let result = vector_add(left, right);
```

当函数及其 `@operator` 语义在当前文件可见时，也可使用运算符语法：

```feng
let result = left + right;
```

compiler 将后一种写法降为对前一种普通函数的调用。`@operator` 不改变函数的参数、返回类型、泛型、可见性、生命周期、ABI 或普通调用方式。

### 2.2 不依附于 type 或 fit

`@operator` 候选设计只标注顶层函数，不标注 `type` 成员方法或 `fit` 方法。

二元运算符描述的是：

```text
(Left, Right) -> Result
```

若强制放入 `type` 或 `fit`，会把该关系人为解释为 `Left.method(Right)`，从而引入以下问题：

1. 必须人为决定运算符属于左操作数还是右操作数。
2. `Scalar * Vector` 与 `Vector * Scalar` 需要不同的 receiver 归属规则。
3. 若同时搜索左右类型及其可见 `fit`，需要额外定义候选优先级和冲突规则。
4. 目标类型转换没有自然的接收者。

顶层函数直接表达全部操作数，不引入 receiver，函数名也为显式调用和文档提供稳定入口。

### 2.3 与属性的边界

索引访问不属于运算符重载。其候选设计已经由 [Feng 语言属性规范草案](../specifications/feng-prop-draft.md) 归入索引属性：

```text
obj.name       -> 普通属性 getter/setter 调用语法糖
obj[index]     -> 索引属性 getter/setter 调用语法糖
left + right   -> @operator 顶层函数调用语法糖
```

本文不重复定义普通属性和索引属性规则。

## 3 候选函数形状

### 3.1 一元运算符

一元运算符函数接收一个操作数：

```feng
@operator("-")
open func vector_negate(value: Vector): Vector {
  // ...
}
```

候选 lowering：

```text
-value -> vector_negate(value)
```

### 3.2 二元运算符

二元运算符函数接收左右两个操作数：

```feng
@operator("*")
open func scalar_vector_multiply(left: float, right: Vector): Vector {
  // ...
}

@operator("*")
open func vector_scalar_multiply(left: Vector, right: float): Vector {
  // ...
}
```

候选 lowering：

```text
scalar * vector -> scalar_vector_multiply(scalar, vector)
vector * scalar -> vector_scalar_multiply(vector, scalar)
```

两个方向是两组独立的参数类型组合，不需要定义类型归属规则。

### 3.3 目标类型转换

讨论中的候选语法使用 `@operator("=")` 表达从源类型进入目标类型的转换：

```feng
@operator("=")
open func string_from_bytes(value: u8[]): String {
  // ...
}
```

当上下文已经明确目标类型为 `String` 时：

```feng
let text: String = bytes;
```

compiler 可根据源类型、目标类型以及当前可见的 `@operator("=")` 函数，将其降为：

```feng
let text: String = string_from_bytes(bytes);
```

这里的返回类型是目标转换运算符的结构条件，不改变普通函数“返回类型不参与重载”的既有规则。`@operator("=")` 是否允许同类型赋值重载、是否只允许一步转换以及允许隐式转换的具体站点，仍属于待决策事项。

## 4 可见性与候选决议

### 4.1 import 驱动的文件级可见性

只有当前文件可见的顶层函数，其 `@operator` 语义才参与当前表达式的候选决议。

```feng
import math.vector_operators;

let result = left + right;
```

`import` 是否存在只影响当前文件，不影响同一模块中的其他文件。运算符能力不做全局注册，也不由 runtime 动态发现。

### 4.2 与普通函数重载分离

普通函数调用继续按函数名及现有函数重载规则决议。运算符表达式则按以下信息收集候选：

1. `@operator` 参数中的运算符符号。
2. 一元或二元运算符要求的参数数量。
3. 当前表达式的操作数静态类型。
4. 目标类型转换场景中已经确定的目标类型。

函数名不参与运算符候选的区分。不同名称的函数只要声明相同运算符并同时适用，仍构成冲突。

### 4.3 多候选冲突

同一表达式若存在多个适用的可见 `@operator` 函数，compiler 必须报冲突，不得按以下方式隐式选择：

1. import 顺序。
2. 声明顺序。
3. 函数名称。
4. 模块名称。
5. 返回类型。

同一个函数声明经多条 re-export 路径进入可见面时，应按声明身份去重，不应误判为多个实现。

泛型候选与具体候选之间是否允许优先级、还是只要多个候选适用就报冲突，尚未确定。

## 5 孤儿运算符规则

### 5.1 目的

导入某个 `@operator` 会改变当前文件中运算符表达式的候选集合。若第三方包可以无限制地向下游传播两个外部类型之间的运算符语义，容易形成跨包冲突和不稳定的表达式含义。

因此，候选设计沿用 `fit` 的孤儿规则方向：孤儿运算符在声明包内正常生效，但其运算符语义不得跨包导出。

### 5.2 二元运算符判定

对于：

```feng
@operator("+")
open func combine(left: A, right: B): R {
  // ...
}
```

候选基线为：

1. 当前包拥有 `A` 或 `B` 中至少一个时，`@operator("+")` 可随函数导出。
2. `A`、`B` 都不属于当前包时，该声明是孤儿运算符。
3. 普通一元、二元运算符的返回类型 `R` 不参与所有权判定，避免仅通过声明本地返回包装类型绕过孤儿限制。

### 5.3 一元运算符判定

一元运算符按其操作数类型判断。若操作数类型不属于当前包，则该 `@operator` 语义属于孤儿运算符。返回类型不参与判定。

### 5.4 目标类型转换判定

目标类型转换同时涉及源类型和目标类型：

```feng
@operator("=")
open func convert(value: Source): Target {
  // ...
}
```

候选基线为：

1. 当前包拥有 `Source` 或 `Target` 中至少一个时，转换运算符语义可导出。
2. `Source`、`Target` 都不属于当前包时，该声明是孤儿转换运算符。

因此，上层库可为自己拥有的 `String` 定义从内建字节值数组进入 `String` 的转换，而 compiler 不需要感知 `String` 的名称或来源。

### 5.5 导出行为

孤儿运算符不使函数声明非法，也不阻止普通函数导出：

```text
声明包内：
- 普通函数调用可用。
- 运算符语法糖可用。

其他包 import 后：
- 普通函数按 open 规则正常可用。
- @operator 映射不可见，运算符语法糖不可用。
```

compiler 在导出符号信息时只移除孤儿函数的 `@operator` 语义，保留函数本身及其普通公开信息。同时应沿用孤儿 `fit` 的处理风格，输出明确的肯定式提示，而不是静默忽略、报错或警告。

类型别名、泛型实例、内建复合类型、tuple、union-form 与本地 `spec` 约束参与所有权判定时的具体归属规则，尚未确定。

## 6 编译器与运行时边界

### 6.1 Parser

当前注解语法已经支持参数列表。若未来实施，parser 需要将 `@operator` 纳入内建注解集合，但函数仍按普通顶层函数解析，不新增运算符声明 AST 类别。

### 6.2 Semantic

semantic 负责：

1. 校验 `@operator` 仅用于允许的顶层函数声明。
2. 校验注解参数是受支持的运算符符号。
3. 校验函数参数数量和返回类型符合对应运算符形状。
4. 在当前文件可见面中收集候选并完成静态决议。
5. 多个候选适用时报告冲突。
6. 判断孤儿运算符，并限制其跨包导出语义。

### 6.3 Codegen

semantic 确定唯一目标后，codegen 直接复用普通函数调用路径。运算符重载不引入动态分派、运行时查表、额外装箱或专用 runtime API。

### 6.4 导出与导入

非孤儿 `@operator` 语义需要随普通函数公开信息导出，以便其他包 import 后在当前文件可见面中使用。孤儿函数只导出普通函数信息，不导出 `@operator` 映射。

## 7 与上层库的关系

`@operator` 是通用语言注解，不绑定任何 `std` 类型。compiler 只检查函数签名和静态类型，不感知实现函数位于标准库还是用户包。

未来若用户层定义 `String`、`Array<T>`、向量、矩阵或其他数值类型，它们都通过普通顶层函数参与同一机制：

```feng
@operator("+")
func string_concat(left: String, right: String): String;

@operator("==")
func string_equal(left: String, right: String): bool;
```

这不要求 compiler 或 runtime 为任何上层类型增加特判。

## 8 待决策事项

以下内容在本轮讨论中尚未确定，不能作为已定规范实施：

1. 首版允许重载的完整运算符集合。
2. `@operator` 参数使用符号字符串还是其他受限形式。
3. 内建运算符已经适用时，是否禁止、忽略或允许用户重载候选参与。
4. `==`、`!=`、比较运算符等是否强制特定返回类型。
5. `&&`、`||` 等具有短路语义的运算符是否允许重载。
6. `=` 是否正式用于目标类型转换，以及同类型赋值是否绝对不可重载。
7. 目标类型转换允许出现在哪些赋值、参数、返回和初始化站点。
8. 是否只允许一步目标类型转换。
9. `+=` 等复合赋值是独立重载，还是由基础运算符和普通赋值展开。
10. 泛型运算符的适用性、约束与多候选冲突规则。
11. 孤儿判定中类型别名、泛型实例、内建复合类型、tuple、union-form 和 `spec` 约束的归属规则。
12. 运算符注解在符号表、依赖产物和 LSP 中的具体持久化与展示形式。
13. 诊断码、诊断文本和冲突定位格式。

## 9 关联文档

- [Feng 语言属性规范草案](../specifications/feng-prop-draft.md)：普通属性与索引属性候选设计。
- [Feng 语言函数规范](../specifications/feng-function.md)：普通函数、重载与调用规则。
- [Feng 模块与包规范](../specifications/feng-module.md)：文件级 import 可见性与公开符号导入规则。
- [Feng 契约适配规范](../specifications/feng-fit.md)：现有孤儿适配与导出限制。
- [Feng 表达式与运算规范](../specifications/feng-expression.md)：当前内建运算符、优先级及表达式语义。
- [Feng 语言核心规范](../specifications/feng-language.md)：关键字、内建注解与核心能力总览。
