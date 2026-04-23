# Feng 语言核心规范

> 本文档是 Feng 语言的总览入口，提供所有特性的简介与索引，不重复描述子文档已涵盖的细节。详细规则请跟随各节链接查阅对应规范文档。
>
> **设计原则基础**: 本文档建立在 [Feng 语言设计原则](./feng-principles.md) 之上。

## 1 设计哲学

Feng 是一门**强类型、静态类型、支持 `spec` 契约与 `fit` 显式适配**的编程语言，秉持极简主义设计理念，语法紧凑、关键字稀少，兼顾脚本语言的简洁性与 C 语言的高效性；支持闭包、函数式编程与结构化异常处理，全程保障内存安全；以 `type` 统一定义具名类型，并通过 `extern fn`、`@fixed` 和调用方式注解实现与 C 语言的安全互操作，底层可无缝映射为 C 语言代码；普通 Feng 对象由运行时自动管理，实现上可采用 RC、GC 或等价策略，`@fixed` 边界上的 ABI 值不受该机制接管；同时支持统一的 `.fb` 二进制包格式分发，可携带自有 ABI 静态库层与 C ABI 兼容层，兼顾源码保护、编译加速与跨语言复用。

## 2 语言特性一览

| 特性 | 简述 |
| --- | --- |
| 强类型 / 静态类型 | 所有类型在编译期确定，禁止隐式转换，无 `any` 类型 |
| `type` 统一类型系统 | 普通类型采用托管引用语义；`@fixed type` 映射 C 兼容固定布局 |
| `spec` / `fit` 显式契约 | `spec` 声明契约形状，`fit` 显式建立"类型满足契约"关系，不做结构隐式匹配 |
| `let` / `var` 绑定 | 不可变 / 可变绑定，支持类型推导与默认零值初始化 |
| 函数与重载 | `fn` 定义函数与方法，支持按参数类型重载，返回值不参与重载 |
| Lambda 与闭包 | Lambda 是函数字面量简写，闭包可捕获外部变量并延长生命周期 |
| 模块系统 | `mod` / `use` 实现多级命名空间与可见性控制 |
| 流程控制 | 条件分支、`if` 表达式、顺序匹配、`while` / `for`、`break` / `continue` |
| 结构化异常 | `throw` / `try` / `catch` / `finally`，异常不得穿越 C ABI 边界 |
| GC 托管内存 | 普通对象由运行时自动管理（RC / GC 或等价策略），`@fixed` 值不受接管 |
| C 互操作 | `extern fn` 声明 C 函数，`@fixed` 标记 ABI 兼容类型与函数 |
| 包分发 | `.fb` 统一包格式，支持闭源复用与跨语言分发 |

## 3 基础语法规则

1. 文件与包扩展名:
    - `.ff`: 源文件（Feng File）
    - `.fm`: 项目或包清单文件（Feng Manifest）
    - `.fi`: 编译后包的公开接口及元信息文件（Feng Interface）
    - `.fb`: 包文件（Feng Bundle）
2. 语句结束符: 分号 `;`
3. 代码块: 统一使用花括号 `{}` 包裹
4. 注释规范:
   - 单行注释: `// 注释内容`
   - 多行注释: `/* 注释内容 */`
5. 代码书写顺序: 模块声明 → 模块导入 → 类型定义 → 函数/业务代码,不可颠倒

## 4 关键字、保留字与内建注解

当前规范共定义 `25` 个关键字、`12` 个保留字和 `6` 个内建注解。

规则说明:

- 关键字: 当前版本已经参与语法,词法阶段会直接识别为关键字 token,不得作为标识符。
- 保留字: 当前版本尚未赋予对应语法能力,但为语言演进预留,当前同样不得作为标识符。
- 注解: 以 `@` 开头,用于补充语义分析与代码生成信息,不属于普通标识符，目前仅支持内建注解。
- 例如: `self` 是关键字,`this` 是保留字。

### 4.1 关键字（共25个）

下表按当前全部规范文档汇总 Feng 语言关键字。当前语法中,`extern` 只与 `fn` 组合使用,用于声明 C 外部函数。

| 关键字 | 含义 |
| --- | --- |
| `type` | 定义 Feng 原生类型,包括对象类型和函数类型。 |
| `spec` | 声明契约形状,定义字段与行为签名边界; 不提供实现体。 |
| `fit` | 显式建立“具体类型满足契约”关系,或为类型补充扩展成员。 |
| `extern` | 与 `fn` 组合使用,表示 C 外部函数声明。 |
| `fn` | 定义 Feng 内部普通函数、成员方法和构造函数; 与 `extern` 组合时表示 C 外部函数声明。 |
| `let` | 声明不可变绑定或不可重新赋值的成员。 |
| `var` | 声明可变绑定、可写成员或可变参数。 |
| `pu` | 声明公开可见性。 |
| `pr` | 声明私有可见性。 |
| `self` | 在 `type` 及 `fit` 块的成员 `fn` 与构造函数中引用当前实例; 由编译器隐式提供,无需在参数列表中显式声明。 |
| `mod` | 声明文件所属模块。 |
| `use` | 导入源码模块、feng 二进制包或 C ABI 兼容包中的公开模块。 |
| `as` | 为 `use` 导入目标声明当前文件内可见的别名。 |
| `if` | 条件分支与条件表达式关键字,也用于顺序匹配形式。 |
| `else` | `if` 分支未命中时的后续分支关键字,可组成 `else if`。 |
| `while` | 条件循环关键字。 |
| `for` | 三段式循环关键字。 |
| `break` | 立即退出当前最近一层循环。 |
| `continue` | 跳过当前轮循环剩余语句并进入下一轮判断。 |
| `try` | 声明异常捕获的受保护代码块。 |
| `catch` | 捕获从对应 `try` 块或其调用链抛出的异常。 |
| `finally` | 声明离开 `try` / `catch` 前必定执行的收尾代码块。 |
| `throw` | 显式抛出异常。 |
| `return` | 从当前函数返回结果或直接结束无返回值函数。 |
| `void` | 表示空无类型,用于无返回值函数语义,不可作为普通值类型使用。 |

说明: `true` 与 `false` 是布尔字面量,不属于关键字。

### 4.2 保留字（共12个）

下列保留字当前语言版本不提供对应语法能力,但也不允许作为标识符使用。

| 保留字 | 说明 |
| --- | --- |
| `class` | 保留字,当前版本不可用,且不得作为标识符。 |
| `struct` | 保留字,当前版本不可用,且不得作为标识符。 |
| `public` | 保留字,当前版本不可用,且不得作为标识符。 |
| `private` | 保留字,当前版本不可用,且不得作为标识符。 |
| `pub` | 保留字,当前版本不可用,且不得作为标识符。 |
| `pro` | 保留字,当前版本不可用,且不得作为标识符。 |
| `get` | 保留字,当前版本不可用,且不得作为标识符。 |
| `set` | 保留字,当前版本不可用,且不得作为标识符。 |
| `this` | 保留字,当前版本不可用,且不得作为标识符。 |
| `interface` | 保留字,当前版本不可用,且不得作为标识符。 |
| `static` | 保留字,当前版本不可用,且不得作为标识符。 |
| `enum` | 保留字,当前版本不可用,且不得作为标识符。 |

### 4.3 所有内建注解（共6个）

下表按当前全部规范文档汇总 Feng 的内建注解。

| 内建注解 | 适用位置 | 含义 |
| --- | --- | --- |
| `@fixed` | `type`、`fn` 和方法声明前 | 表达“该声明希望进入 ABI 固定边界”的语义资格。对象形式的 `@fixed type` 可声明 C 兼容结构体或联合体; 函数形式的 `@fixed type` 可声明 C 兼容函数指针类型; `@fixed fn` 或 `@fixed` 方法可声明 Feng 实现的 ABI 兼容函数。`@fixed` 不改变语法形式,其合法性由语义分析检查。 |
| `@union` | 对象形式的 `@fixed type` 声明前 | 将对象形式的 `@fixed type` 声明为 C 联合体; 未标注时默认按 C 结构体处理。 |
| `@cdecl` | 无函数体的 `extern fn` 声明前,或 `@fixed fn` / `@fixed` 方法定义前 | 带一个参数时写作 `@cdecl("libname_or_path")`,表示该无函数体 `extern fn` 从指定原生库导入且使用 `cdecl`; 该参数也可引用当前 `mod` 作用域中以字符串字面量初始化的 `let` 绑定。无参数时用于显式声明 `@fixed fn` 或 `@fixed` 方法的 C ABI 调用方式; 未显式标注时默认按 `cdecl` 处理。 |
| `@stdcall` | 无函数体的 `extern fn` 声明前,或 `@fixed fn` / `@fixed` 方法定义前 | 带一个参数时写作 `@stdcall("libname_or_path")`,表示该无函数体 `extern fn` 从指定原生库导入且使用 `stdcall`; 该参数也可引用当前 `mod` 作用域中以字符串字面量初始化的 `let` 绑定。无参数时用于显式声明 `@fixed fn` 或 `@fixed` 方法使用 `stdcall`。 |
| `@fastcall` | 无函数体的 `extern fn` 声明前,或 `@fixed fn` / `@fixed` 方法定义前 | 带一个参数时写作 `@fastcall("libname_or_path")`,表示该无函数体 `extern fn` 从指定原生库导入且使用 `fastcall`; 该参数也可引用当前 `mod` 作用域中以字符串字面量初始化的 `let` 绑定。无参数时用于显式声明 `@fixed fn` 或 `@fixed` 方法使用 `fastcall`。 |
| `@bounded` | 仅允许出现在编译器生成的 `.fi` 接口文件中 | 表达公开 `let` 绑定的显式绑定元信息: `.fi` 不保留值本身,也不保留 `fn` 实现; 无参数形式标注公开 `let` 成员在成员声明及绑定初值阶段已绑定,带参数形式标注某个公开构造函数会在构造阶段绑定列出的公开 `let` 成员; 不属于普通 `.ff` 源码可手写语法。 |

## 5 模块系统

Feng 使用 `mod` 声明文件所属模块，使用 `use` 导入外部模块或二进制包，支持多级命名空间与可见性控制。详细规则见 [Feng 语言模块系统规范](./feng-module.md)。

- 模块默认私有（`pr mod`），公开须显式声明 `pu mod`
- `use` 引入目标模块的公开 `type`、顶层 `fn` 与模块级 `let` / `var`；支持 `use ... as 别名` 消除冲突
- 同一 `mod` 可分布在多个文件，合并后类型名唯一、顶层 `fn` 可形成重载集合
- 模块级变量先建立默认值存储，再按依赖顺序执行初始化；一个模块只初始化一次

## 6 C 互操作

Feng 通过 `extern fn` 声明 C 外部函数，通过 `@fixed` 标记 ABI 兼容类型与函数，通过调用方式注解指定库来源与调用约定。详细规则见 [Feng 语言 C 互操作规范](./feng-interop.md)。

- `@fixed type`：定义 C 兼容结构体（加 `@union` 可改为联合体）或 C 函数指针类型
- `extern fn`：声明 C 外部函数，须配合带参数的 `@cdecl("lib")` / `@stdcall("lib")` / `@fastcall("lib")`
- `@fixed fn`：定义 Feng 实现的 ABI 兼容回调；`pu @fixed fn` 定义公开 C ABI 导出函数
- `@fixed` 不改变 Feng 侧使用语义，仅约束 C ABI 边界的布局与类型资格

## 7 类型系统与对象模型

Feng 以 `type` 统一定义具名类型，包括对象类型、函数类型与 `@fixed` C 兼容类型；`spec` 声明契约形状，`fit` 显式建立适配关系；`let` / `var` 控制成员与变量的可变性。详细规则见 [Feng 语言类型规范](./feng-type.md)、[变量绑定与作用域规范](./feng-binding.md)、[`spec` 规范](./feng-spec.md)、[`fit` 规范](./feng-fit.md)。

- 基础类型：整数（`i8`～`i64` / `u8`～`u64`，`int` = `i64`）、浮点（`f32` / `f64`，`float` = `f64`）、`bool`、`string`、数组（`T[]`，支持多维 `T[][]` 等）、C 指针（`*T`）
- 普通 `type` 采用托管引用语义；`@fixed type` 映射 C 兼容固定布局，两者 Feng 侧用法相同
- 实例创建遵循三阶段流程：成员声明及绑定初值 → 构造函数 → 对象字面量初始化
- 契约匹配仅通过显式 `spec` / `fit` 关系成立，不做结构隐式匹配
- 类型成员默认公开，使用 `pr` 收窄可见性；`self` 由编译器隐式提供

## 8 函数、Lambda 与闭包

Feng 使用 `fn` 定义函数、成员方法与构造函数；Lambda 是函数字面量的简写形式；闭包可捕获外部变量。详细规则见 [Feng 语言函数系统规范](./feng-function.md)。

- 顶层 `fn`、成员方法与构造函数均可按"名称 + 参数类型"形成重载；返回值不参与重载
- 函数参数须显式标注类型，`let` / `var` 可省略（省略时默认 `let`）
- 程序唯一入口为 `main(args: string[])`
- Lambda 不是独立类型定义；闭包捕获的变量由 GC 跟踪生命周期，不可用于 C 互操作

## 9 表达式与运算

Feng 的表达式系统覆盖字面量、调用、成员访问、下标访问、一元 / 二元运算与赋值语句。详细规则见 [Feng 语言表达式与运算规范](./feng-expression.md)。

- 从左到右求值顺序；`&&` / `||` 短路求值
- `=` 是赋值语句，不参与普通表达式优先级；不同数值类型必须显式转换
- `type` 对象与动态数组的 `==` / `!=` 默认比较引用身份

## 10 流程控制

Feng 支持条件分支、`if` 表达式、顺序匹配与多种循环形式。详细规则见 [Feng 语言流程控制规范](./feng-flow.md)。

- `if / else if / else` 条件分支；`if` 亦可作为表达式求值
- `if 目标值 { 标签: 结果, ... else: ... }` 顺序匹配形式
- `while` 条件循环；`for 初始化; 条件; 更新 { }` 三段式循环；`break` / `continue` 控制转移

## 11 异常模型

Feng 使用 `throw` 显式抛出异常，使用 `try / catch / finally` 捕获与收尾。详细规则见 [Feng 语言异常模型规范](./feng-exception.md)。

- `finally` 在任何离开 `try` 路径（含 `return` / `break` / `continue`）前必定执行；`finally` 中禁止 `return`、`break`、`continue`、`throw`
- 异常不得穿越 `@fixed` ABI 边界；若到达边界未捕获，运行时直接终止进程

## 12 自动内存管理

Feng 对托管对象提供自动内存管理（RC / GC 或等价策略），与 `@fixed` ABI 边界上的非托管内存严格分离。详细规则见 [Feng 语言 GC 与内存模型规范](./feng-gc.md)。

- 托管对象：`string`、动态数组、Feng 原生 `type` 对象、闭包捕获环境
- 非托管：`@fixed type`、`*T` 指针、C 侧分配内存；持有外部资源须显式释放
- 当前版本不提供析构器、`pin` 或 `borrow` 机制

## 13 包分发

Feng 采用统一的 `.fb` 包格式，支持二进制分发、闭源复用与跨语言使用。详细规则见 [Feng 语言包分发规范](./feng-package.md)。

- `.fb` 可选携带 feng 自有 ABI 静态库层（`lib/`）和 C ABI 兼容库层（`clib/` + `include/`）
- 公开接口通过 `.fi` 文件描述，由编译器自动生成，不含实现代码
- `feng.fm` 清单文件记录包名、版本、平台、能力层与直接依赖

## 14 完整代码示例

### 14.1 c_interop.ff

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
type PointCB(p: Point);

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

### 14.2 main.ff

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
