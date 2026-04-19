# Feng 语言流程控制规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的流程控制概要说明,聚焦 feng 语言的条件判断、模式匹配、循环与异常处理规则。

## 1 流程控制概览

- Feng 提供条件分支、模式匹配、循环和结构化异常处理。
- 条件判断支持 `if / else if / else` 形式。
- `if` 支持作为表达式使用,用于分支匹配赋值。
- 循环支持 `while`、`for`、`break` 和 `continue`。
- 异常处理支持 `try / catch / finally` 结构。

## 2 普通条件判断

```feng
if a > b {
    // 逻辑代码
} else if a == b {
    // 逻辑代码
} else {
    // 逻辑代码
}
```

## 3 if模式匹配

`if` 可作为表达式赋值,支持分支匹配,默认匹配 `else` 分支。

```feng
let stage = if age {
    0: "婴儿",
    18: "成年",
    60: "老年",
    else: "青年"
};
```

## 4 循环语句

- 支持 `while`、`for` 循环。
- 支持 `break` 跳出循环。
- 支持 `continue` 跳过当前循环。

## 5 异常处理try/catch/finally

- `try`: 包裹可能触发异常的代码。
- `catch`: 捕获异常并执行异常处理逻辑。
- `finally`: 无论是否发生异常、是否 `return`,均会执行。
- 常用于资源释放和收尾操作。

```feng
fn test() {
    try {
        let arr = [1, 2, 3];
        let num = arr[5]; // 数组越界异常
    } catch {
        print("程序发生异常");
    } finally {
        print("执行收尾操作");
    }
}
```

## 6 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、流程控制概要、模块、类型、函数、C 互操作、GC、包分发与完整示例。
- 本文档: 条件判断、模式匹配、循环和异常处理规则的独立补充文档。
