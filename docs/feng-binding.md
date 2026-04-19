# Feng 语言变量绑定与作用域规范

本文档用于补充 [feng-language.md](./feng-language.md) 中关于 `let` / `var` 的概要说明,聚焦 Feng 语言中的变量绑定、参数变更性和块作用域规则。

## 1 绑定规则概览

- `let` 用于声明不可变绑定,声明后不可再次赋值。
- `var` 用于声明可变绑定,声明后允许在当前作用域内修改其值。
- 函数参数必须显式标注类型,但可省略 `let` / `var`; 省略时默认按 `let` 不可变参数处理。
- 模块内变量、函数内局部变量和 `type` / `extern type` 的成员变量必须显式书写 `let` 或 `var`,不可省略。
- `let` 与 `var` 都遵循块作用域规则,子级块中的同名变量会优先屏蔽上层块中的绑定。

## 2 `let` 与 `var` 的基本语义

`let` 用于声明只读绑定,适合不会在后续流程中被重新赋值的值; `var` 用于声明可写绑定,适合需要在作用域内更新状态的值。两者都只是绑定规则的声明方式,不改变变量的类型检查、类型推导或默认零值规则。

示例:

```feng
// 不可变绑定
let name = "feng";

// 可变绑定
var count = 0;
count = count + 1;
```

规则说明:

- `let` 绑定在初始化之后不可再次赋值。
- `var` 绑定允许在当前作用域内重新赋值。
- 有初始值时,`let` / `var` 均可省略类型标注,由编译器自动推导。
- 无初始值时,`let` / `var` 均需显式标注类型,再按对应类型执行默认零值初始化。

## 3 不同位置的声明规则

### 3.1 模块内变量与函数内局部变量

模块内变量与函数体内局部变量都必须显式写出 `let` 或 `var`,不可省略绑定方式。这样可以在语法层面直接区分只读绑定与可变绑定,避免局部状态语义含糊。

```feng
pu mod demo.binding;

let app_name = "feng";
var total: int;

fn main(args: string[]) {
    let user = "guest";
    var retry = 0;
    retry = retry + 1;
}
```

补充规则:

- 模块内变量和函数内局部变量都不能只写变量名与类型,必须带 `let` 或 `var`。
- 是否可省略类型取决于是否提供初始值,不影响 `let` / `var` 的必写要求。

### 3.2 函数参数

函数参数必须显式标注类型,但绑定方式可以省略。参数省略 `let` / `var` 时,默认等价于 `let` 参数,函数体内不可直接修改其值; 如需修改参数,必须显式写为 `var` 参数。

```feng
fn sum(a: int, b: int): int {
    return a + b;
}

fn increase(step: int, var total: int) {
    total = total + step;
}

fn visit(let name: string) {
    print(name);
}
```

补充规则:

- 参数即使省略 `let` / `var`,也不能省略类型。
- `Lambda` 参数沿用相同规则: 可省略 `let` / `var`,省略时默认按 `let` 处理。

### 3.3 类型成员

`type` 与对象形式的 `extern type` 成员变量都必须显式写出 `let` 或 `var`,不可省略。成员的可变性由成员自身的 `let` / `var` 决定,可与 `pu` / `pr` 访问控制组合使用。

```feng
type User {
    var name: string;
    pr let id: int;
}

extern type Point {
    var x: int;
    var y: int;
}
```

## 4 块作用域与名称屏蔽

`let` 与 `var` 都支持块作用域。一个绑定只在其声明所在的块内及其子块中可见; 当子级块中声明了同名变量时,编译器总是优先解析到最近作用域中的那个绑定,从而屏蔽外层同名变量。

```feng
fn show_scope(name: string) {
    let label = "outer";

    if name != "" {
        var label = "inner";
        label = label + "-block";
        print(label);
    }

    print(label);
}
```

规则说明:

- 子级块可以重新声明与外层同名的 `let` 或 `var` 绑定。
- 名称冲突时,最近作用域中的绑定优先级更高。
- 子级块结束后,外层被屏蔽的同名变量重新可见。
- 是否允许重新赋值只由当前实际引用到的绑定是 `let` 还是 `var` 决定。

## 5 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、绑定规则概要、模块、类型、函数、C 互操作、流程控制、异常、GC、包分发与完整示例。
- [feng-type.md](./feng-type.md): 数据类型、默认零值、类型声明与类型约束。
- [feng-function.md](./feng-function.md): 普通函数、参数规则、入口函数、Lambda 与闭包。
- 本文档: `let` / `var` 的绑定语义、参数默认规则以及块作用域与名称屏蔽规则。
