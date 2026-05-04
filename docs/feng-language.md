# Feng 语言核心规范

> 本文档是 Feng 语言的总览入口，提供所有特性的简介与索引。详细规则请跟随各节链接查阅对应规范文档；本文档本身不重复规则细节。
>
> **设计原则基础**: 本文档建立在 [Feng 语言设计原则](./feng-principles.md) 之上。

## 1 设计哲学

Feng 是一门**强类型、静态类型、支持 `spec` 契约与 `fit` 显式适配**的编程语言，秉持极简主义设计理念，语法紧凑、关键字稀少，兼顾脚本语言的简洁性与 C 语言的高效性；支持闭包、函数式编程与结构化异常处理，全程保障内存安全；以 `type` 统一定义具名类型，并通过 `extern fn`、`@fixed` 和调用方式注解实现与 C 语言的安全互操作，底层可无缝映射为 C 语言代码；普通 Feng 对象由运行时自动内存管理，`@fixed` 边界上的 ABI 值不受该机制接管；同时支持统一的 `.fb` 二进制包格式分发，可携带自有 ABI 静态库层与 C ABI 兼容层，兼顾源码保护、编译加速与跨语言复用。

## 2 语言特性一览

| 特性 | 简述 |
| --- | --- |
| 强类型 / 静态类型 | 所有类型在编译期确定，所有类型转换都必须显式写出，无 `any` 类型 |
| `type` 统一类型系统 | 普通类型采用托管引用语义；`@fixed type` 映射 C 兼容固定布局 |
| `spec` / `fit` 显式契约 | `spec` 声明契约形状，`fit` 显式建立"类型满足契约"关系，不做结构隐式匹配 |
| `let` / `var` 绑定 | 不可变 / 可变绑定，支持类型推导与默认零值初始化 |
| 函数与重载 | `fn` 定义函数与方法，支持按参数类型重载，返回值不参与重载 |
| Lambda 与闭包 | Lambda 是函数字面量简写，闭包可捕获外部变量并延长生命周期 |
| 模块系统 | `mod` / `use` 实现多级命名空间与可见性控制 |
| 流程控制 | 条件分支、`if` 表达式、条件匹配、`while` / `for`、`break` / `continue` |
| 结构化异常 | `throw` / `try` / `catch` / `finally`，异常不得穿越 C ABI 边界 |
| 自动内存管理 | 普通对象由运行时自动管理，`@fixed` 值不受接管，详见 [feng-lifetime.md](./feng-lifetime.md) |
| C 互操作 | `extern fn` 声明 C 函数，`@fixed` 标记 ABI 兼容类型与函数 |
| 包分发 | `.fb` 统一包格式，支持闭源复用与跨语言分发 |

文件扩展名:

- `.ff`: 源文件（Feng File）
- `.fm`: 项目或包清单文件（Feng Manifest）
- `.ft`: 编译器导出的符号表文件（Feng Symbol Table）；公开包表位于 `.fb/mod/**/*.ft`，本地缓存位于 `build/obj/symbols/**/*.ft`
- `.fb`: 包文件（Feng Bundle）

语法约定:

- 语句结束符: 分号 `;`
- 代码块: 统一使用花括号 `{}` 包裹
- 文档注释: `/** 注释内容 */`
- 普通注释: `// 注释内容`；`/* 注释内容 */`
- 文件内书写顺序: 模块声明 → 模块导入 → 类型定义 → 函数/业务代码

## 3 关键字、保留字与内建注解

当前规范共定义 `26` 个关键字、`23` 个保留字和 `6` 个内建注解及 `17` 个内建类型名及别名。

- **关键字**: 词法阶段直接识别为 token，不得作为标识符。
- **保留字**: 当前版本暂无对应语法，同样不得作为标识符。
- **内建注解**: 以 `@` 开头，用于补充语义分析与代码生成信息，目前仅支持内建注解。

### 3.1 关键字（共26个）

| 关键字 | 用途简述 |
| --- | --- |
| `type` | 定义 Feng 原生具名类型 |
| `spec` | 声明契约形状（字段与行为签名），不提供实现体 |
| `fit` | 显式建立"类型满足契约"关系，或为类型补充扩展成员 |
| `extern` | 与 `fn` 组合，声明 C 外部函数 |
| `fn` | 定义函数、成员方法与构造函数；与 `extern` 组合时声明 C 外部函数 |
| `let` | 声明不可变绑定或不可再赋值的成员 |
| `var` | 声明可变绑定、可写成员或可变参数 |
| `pu` | 公开可见性 |
| `pr` | 私有可见性 |
| `self` | 在 `type` 及 `fit` 块的成员方法与构造函数中引用当前实例，由编译器隐式提供 |
| `mod` | 声明文件所属模块 |
| `use` | 导入源码模块、feng 二进制包或 C ABI 兼容包中的公开模块 |
| `as` | 为 `use` 导入目标声明当前文件内可见的别名 |
| `if` | 条件分支、条件表达式与条件匹配 |
| `else` | `if` 未命中时的后续分支，可组成 `else if` |
| `while` | 条件循环 |
| `for` | 三段式循环与 `for/in` 迭代循环 |
| `in` | `for/in` 迭代循环中分隔循环变量与序列的关键字 |
| `break` | 立即退出当前最近一层循环 |
| `continue` | 跳过当前轮循环剩余语句并进入下一轮 |
| `try` | 声明受保护代码块 |
| `catch` | 捕获 `try` 块或其调用链抛出的异常 |
| `finally` | 声明离开 `try`/`catch` 前必定执行的收尾代码块 |
| `throw` | 显式抛出异常 |
| `return` | 从当前函数返回结果或结束无返回值函数 |
| `void` | 无返回值语义，不可作为普通值类型使用 |

### 3.2 内建类型及别名

以下词汇用于类型书写，便于开发者快速了解语言内建类型词表。

- 内建类型名: `12` 个
- 类型别名: `5` 个

| 内建类型名 | 别名 | 说明 |
| --- | --- | --- |
| `i8` | - | 8 位有符号整数 |
| `i16` | - | 16 位有符号整数 |
| `i32` | `int` | 32 位有符号整数 |
| `i64` | `long` | 64 位有符号整数 |
| `u8` | `byte` | 8 位无符号整数 |
| `u16` | - | 16 位无符号整数 |
| `u32` | - | 32 位无符号整数 |
| `u64` | - | 64 位无符号整数 |
| `f32` | `float` | 单精度浮点数 |
| `f64` | `double` | 双精度浮点数 |
| `bool` | - | 布尔类型 |
| `string` | - | UTF-8 字符串 |

说明:

- `int`、`long`、`byte`、`float`、`double` 只是别名，不引入新的类型实体。
- 详细语义见 [Feng 内建类型规范](./feng-builtin-type.md) 与 [Feng 语言表达式与运算规范](./feng-expression.md)。

### 3.3 字面量

以下仅列出会占用标识符命名空间的字面量词。

- 布尔字面量词: `2` 个（`true`、`false`）

| 类别 | 词/形式 | 数量 |
| --- | --- | --- |
| 布尔字面量词 | `true`、`false` | 2 |

### 3.4 保留字（共23个）

| 保留字 | 说明 |
| --- | --- |
| `class` | 保留，当前不可用 |
| `struct` | 保留，当前不可用 |
| `public` | 保留，当前不可用 |
| `private` | 保留，当前不可用 |
| `protected` | 保留，当前不可用 |
| `internal` | 保留，当前不可用 |
| `pub` | 保留，当前不可用 |
| `pro` | 保留，当前不可用 |
| `get` | 保留，当前不可用 |
| `set` | 保留，当前不可用 |
| `this` | 保留，当前不可用 |
| `interface` | 保留，当前不可用 |
| `static` | 保留，当前不可用 |
| `enum` | 保留，当前不可用 |
| `const` | 保留，当前不可用 |
| `abstract` | 保留，当前不可用 |
| `char` | 保留，当前不可用 |
| `is` | 保留，当前不可用 |
| `of` | 保留，当前不可用 |
| `switch` | 保留，当前不可用 |
| `case` | 保留，当前不可用 |
| `export` | 保留，当前不可用 |
| `import` | 保留，当前不可用 |

### 3.5 内建注解（共6个）

详细用法见 [Feng 语言 C 互操作规范](./feng-interop.md) 与 [Feng 语言包分发规范](./feng-package.md)。

| 内建注解 | 适用位置 | 用途简述 |
| --- | --- | --- |
| `@fixed` | `type`、`fn`、方法声明前 | 标注该声明进入 ABI 固定边界；不改变 Feng 侧语法形式，合法性由语义分析检查 |
| `@union` | 对象形式的 `@fixed type` 声明前 | 将 `@fixed type` 声明为 C 联合体；未标注时默认按 C 结构体处理 |
| `@cdecl` | `extern fn` 声明前，或 `@fixed fn`/`@fixed` 方法定义前 | 指定 `cdecl` 调用方式；带参数时用于 `extern fn` 导入并指定库来源，无参数时用于 `@fixed fn` |
| `@stdcall` | `extern fn` 声明前，或 `@fixed fn`/`@fixed` 方法定义前 | 同上，`stdcall` 版本 |
| `@fastcall` | `extern fn` 声明前，或 `@fixed fn`/`@fixed` 方法定义前 | 同上，`fastcall` 版本 |
| `@bounded` | 仅作为编译器导出的符号表语义元信息存在（公开 `.ft` / 本地缓存 `.ft`） | 标注公开 `let` 绑定的显式绑定事实，不属于可手写语法 |

## 4 模块系统

Feng 用 `mod` 声明文件所属模块，用 `use` 导入外部模块或二进制包，支持多级命名空间与可见性控制。详细规则见 [Feng 语言模块系统规范](./feng-module.md)。

## 5 C 互操作

Feng 通过 `extern fn` 声明 C 外部函数，通过 `@fixed` 标记 ABI 兼容类型与函数，通过调用方式注解指定库来源与调用约定。详细规则见 [Feng 语言 C 互操作规范](./feng-interop.md)。

## 6 类型系统与对象模型

Feng 以 `type` 统一定义具名类型；`spec` 声明契约形状；`fit` 显式建立适配关系；`let`/`var` 控制成员与变量的可变性。构造函数与终结器作为 `type` 的特殊成员，其声明与约束也统一见 [Feng 语言类型规范](./feng-type.md)。详细规则分别见 [Feng 语言类型规范](./feng-type.md)、[Feng 语言变量绑定与作用域规范](./feng-binding.md)、[Feng 语言 `spec` 规范](./feng-spec.md)、[Feng 语言 `fit` 规范](./feng-fit.md)。

## 7 函数、Lambda 与闭包

Feng 用 `fn` 定义函数与成员方法；构造函数与终结器作为 `type` 的特殊成员，规则见 [Feng 语言类型规范](./feng-type.md)；Lambda 是函数字面量的简写形式；闭包可捕获外部变量。其余函数规则见 [Feng 语言函数规范](./feng-function.md)。

## 8 表达式与运算

Feng 的表达式系统覆盖字面量、调用、成员访问、下标访问、一元/二元运算与赋值语句；赋值语句同时包含 `=` 以及数值/位运算复合赋值。详细规则见 [Feng 语言表达式与运算规范](./feng-expression.md)。

## 9 流程控制

Feng 支持条件分支、`if` 表达式、条件匹配与多种循环形式。详细规则见 [Feng 语言流程控制规范](./feng-flow.md)。

## 10 异常模型

Feng 用 `throw` 显式抛出异常，用 `try`/`catch`/`finally` 捕获与收尾。详细规则见 [Feng 语言异常模型规范](./feng-exception.md)。

## 11 自动内存管理

Feng 对托管对象提供自动内存管理，与 `@fixed` ABI 边界上的非托管内存严格分离。终结器的声明规则见 [Feng 语言类型规范](./feng-type.md)，生命周期与执行流程见 [Feng 语言对象生命周期规范](./feng-lifetime.md)。

## 12 包分发

Feng 采用统一的 `.fb` 包格式，支持二进制分发、闭源复用与跨语言使用。详细规则见 [Feng 语言包分发规范](./feng-package.md)。

## 13 完整代码示例

### 13.1 c_interop.ff

```feng
pu mod libc.interop;

// C 兼容结构体
@fixed
type Point {
  var x: int;
  var y: int;
}

// C 兼容函数指针类型
@fixed
spec PointCB(p: Point): void;

let point_lib = "./libpoint.so";

// 声明 C 外部函数
@cdecl(point_lib)
extern fn point_add(p1: Point, p2: Point): Point;

@cdecl(point_lib)
extern fn exec_point_cb(p: Point, cb: PointCB);

// 定义 feng 回调函数
@fixed
fn on_point(p: Point) {
  print("Point:x=", p.x, " y=", p.y);
}
```

### 13.2 main.ff

```feng
mod main;
use libc.interop;
use mylib; // 导入 feng 自有二进制包

// 函数契约形状
spec IntToInt(x: int): int;

// feng 内部普通函数
fn make_adder(base: int): IntToInt {
  return (x: int) -> base + x;
}

fn main(args: string[]) {
  // 操作 C 兼容结构体
  let p1 = Point {x: 10, y: 20};
  let p2 = Point {x: 5, y: 5};
  let res_p = point_add(p1, p2);
  print(res_p.x, res_p.y);

  // 传递 feng 回调给 C
  exec_point_cb(res_p, on_point);

  // 调用 feng 自有包成员
  let user = User {name: "test", age: 20};
  add_user(user);

  // 闭包调用
  let add5 = make_adder(5);
  print(add5(3));

  // 数组操作
  let arr = [10, 20, 30];
  print(arr[0]);

  // 异常处理
  try {
    let num = arr[10];
  } catch {
    print("数组越界异常");
  } finally {
    print("程序执行完毕");
  }
}
```
