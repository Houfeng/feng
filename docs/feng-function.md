# Feng 语言函数规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的函数系统概要说明,聚焦 feng 语言的普通函数、程序入口函数、Lambda 表达式与闭包规则。

## 1 函数系统概览

- Feng 使用 `fn` 定义内部普通函数、成员方法和构造函数。
- 函数与成员方法支持 `pr` / `pu` 可见性控制。
- 程序唯一入口为 `main(args: string[])`。
- `Lambda` 是函数实现或函数字面量的一种简写形式,不是独立的函数类型定义。
- 闭包支持捕获外部作用域变量,其可用位置和可赋值性由目标函数类型决定。

## 2 Feng 内部普通函数

用于 feng 内部的一般函数形式,支持闭包与变量捕获,可使用所有 feng 类型。

规则说明:

- `void` 代表空无类型; 无返回值时可省略不写,有返回值时可自动推导,也可显式声明。
- 函数类型声明必须位于模块级顶层; 同一模块内允许前向引用,可在后续位置补充声明。

```feng
// 无返回值,void 省略
fn test(x: int, var y: int) {
    y = y + 1;
}

// 自动推导返回 int 类型
fn add(a: int, b: int) {
    return a + b;
}
```

## 3 函数参数规则

- 所有函数参数都必须显式标注类型,不可省略或推导。
- 参数可省略 `let` / `var`; 省略时默认等价于 `let` 参数,函数体内不可直接修改参数值。
- 如需显式声明参数不可变,可写为 `let` 参数。
- 需要在函数体内修改参数时,必须显式使用 `var` 声明可变参数。
- `Lambda` 参数同样必须显式标注类型; 参数变更性规则与普通函数一致。

模块内变量、函数内局部变量的 `let` / `var` 必写规则,以及块作用域与名称屏蔽规则,见 [Feng 语言变量绑定与作用域规范](./feng-binding.md)。

参数示例:

```feng
fn sum(a: int, b: int): int {
    return a + b;
}

fn increase(step: int, var total: int) {
    total = total + step;
}
```

## 4 函数可见性

- 函数可见性遵循 `pr` / `pu` 规则。
- 顶层 `fn` 默认等价于 `pr fn`,仅当前模块内可见。
- 需要对外暴露时,必须显式使用 `pu fn`。
- 成员方法同样可使用 `pr` / `pu` 控制访问范围。

可见性示例:

私有顶层函数:

```feng
fn helper(x: int): int {
    return x + 1;
}
```

公开顶层函数:

```feng
pu fn add(a: int, b: int): int {
    return a + b;
}
```

成员方法可见性:

```feng
type User {
    pu fn info(): string {
        return "user";
    }

    pr fn normalize_name(name: string): string {
        return name;
    }
}
```

## 5 程序入口函数

程序唯一入口为 `main` 函数,必须接收 `string[]` 类型的 `args` 命令行参数。

```feng
fn main(args: string[]) {
    // 程序执行入口
}
```

## 6 Lambda表达式与闭包

- `Lambda` 不是独立的函数类型定义,而是函数实现或函数字面量的一种简写形式; 其可用位置和可赋值性仍由目标函数类型决定。
- 仅单行 `Lambda` 使用 `->` 连接参数与表达式,省略 `{}` 和 `return`。
- `Lambda` 参数必须显式标注类型。
- 支持捕获外部作用域变量,形成闭包,GC 自动管理闭包内存。

```feng
// 单行 Lambda
let func = (x: int) -> x * 2;

// 闭包示例
type IntToInt(x: int): int;

fn make_adder(base: int): IntToInt {
    // 捕获外部变量 base,形成闭包
    return (x: int) -> base + x;
}
```

## 7 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、函数系统概要、模块、类型、C 互操作、流程控制、异常、GC、包分发与完整示例。
- 本文档: 普通函数、入口函数、Lambda 与闭包规则的独立补充文档。
