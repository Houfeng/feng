# Feng 语言异常模型规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的异常处理概要说明,聚焦 Feng 语言中的 `throw`、异常传播、`try/catch/finally` 与 C ABI 边界规则。

> **设计原则基础**: `extern fn` 的限制属于语法层; `@fixed` 边界上的异常限制属于语义与代码生成层。详见 [Feng 语言设计原则](./feng-principles.md)。

## 1 异常模型概览

- Feng 支持结构化异常处理,使用 `throw` 触发异常,使用 `try/catch/finally` 处理异常。
- 异常沿调用栈向外传播,直到被最近的 `catch` 捕获。
- `finally` 在离开 `try` 作用域时总会执行,无论原因是正常结束、`return`、`break`、`continue` 还是异常。
- 异常不能跨越 C ABI 边界传播。

## 2 `throw` 语句

`throw 表达式;` 用于显式抛出异常。

规则说明:

- `throw` 后必须跟一个非 `void` 表达式。
- 可抛出的值限于 Feng 可管理的普通值,如基础类型、`string`、数组或 Feng 原生 `type` 对象。
- `@fixed type`、`@fixed @union type`、`*T` 指针以及其他 C 侧非托管值不可直接作为异常值抛出。
- `throw` 本身会终止当前执行路径,其后的同级语句不可达。

```feng
fn ensure_positive(x: int) {
    if x < 0 {
        throw "x must be positive";
    }
}
```

## 3 `try/catch/finally` 语义

### 3.1 `try`

- `try` 用于包裹可能抛出异常的代码块。
- `try` 块内一旦抛出异常,后续未执行的语句会被中断。

### 3.2 `catch`

- `catch` 捕获从对应 `try` 块或其内部调用链抛出的异常。
- 当前语言版本仅支持无绑定形式的 `catch { ... }`,表示捕获任意 Feng 异常。
- 一个 `try` 块最多跟随一个 `catch` 块。
- `catch` 块可省略；`try { ... } finally { ... }` 不带 `catch` 合法，异常仍会向上传播，`finally` 保证执行。

### 3.3 `finally`

- `finally` 用于收尾逻辑,在离开 `try`/`catch` 路径前必定执行。
- `finally` 中禁止使用 `return`、`break`、`continue` 和新的 `throw`,以避免覆盖既有控制流结果。

```feng
fn read_value() {
    try {
        let arr = [1, 2, 3];
        let num = arr[10];
    } catch {
        print("读取失败");
    } finally {
        print("执行收尾操作");
    }
}

// 不带 catch，异常向上传播，finally 保证执行
fn with_cleanup() {
    try {
        do_work();
    } finally {
        release_resource();
    }
}
```

## 4 异常传播规则

- 未被当前函数处理的异常会继续向调用方传播。
- 若异常传播到程序入口 `main` 仍未被捕获,程序立即以异常失败状态结束。
- `catch` 处理完成后,控制流从整个 `try/catch/finally` 结构之后继续执行。
- `return`、`break`、`continue` 在离开 `try` 前也必须先执行对应的 `finally`。

### 4.1 异常路径上的资源清理

- 异常沿调用栈传播时,栈上所有已经初始化、当前作用域可见的托管局部(由编译器与运行时共同管理引用计数的对象,例如字符串、数组、对象实例)必须被释放,等价于这些局部正常走完作用域。
- 释放顺序与正常退出相同:按声明的逆序逐个释放,且每个槽位释放后被置空,任何后续路径都不会重复释放。
- 抛出表达式自身的值在传递到 `catch` 之前持有 +1 引用;`catch` 块结束后由运行时释放该引用。`catch` 体内若再次抛出(语言后续阶段支持),适用相同规则。
- 任何被 `@fixed` ABI 边界拦截或在程序入口外仍未被捕获的异常,在终止进程前同样要释放沿途所有可见的托管局部,以保证调试信息一致、不掩盖真实泄漏。

## 5 与 C ABI 的边界约束

- 异常不得穿过 `@fixed` ABI 边界传播到 C 调用方。
- `@fixed fn` 回调函数、`@fixed` 方法和 `pu @fixed fn` 导出函数内部若可能抛出异常,必须在函数体内捕获并转换为 C 侧可理解的返回值或错误码。
- 编译器在语义阶段对 `@fixed` ABI 边界做静态异常传播分析：若 `@fixed fn`、`@fixed` 方法或 `pu @fixed fn` 内存在可能未被捕获而到达函数出口的异常路径，编译期报错，而非依赖运行时检测。
- 若未捕获异常到达 `@fixed` 的 ABI 边界,运行时直接终止当前进程,不得向 C 侧继续返回未定义状态。
- `extern fn` 导入声明本身不向 Feng 异常系统抛出可捕获异常; 来自 C 的错误应通过返回值、错误码或显式回调约定传递。

## 6 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、异常处理概要、流程控制、函数、GC、C 互操作与包分发。
- [feng-flow.md](./feng-flow.md): `if`、循环、`break` / `continue` 与 `try/catch/finally` 的控制流关系。
- [feng-interop.md](./feng-interop.md): `extern fn` 导入规则、`@fixed` 的 ABI 规则与异常边界。
- 本文档: `throw`、传播模型、`finally` 执行语义与 C ABI 边界限制。
