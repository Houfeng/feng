# 异常处理

Feng 使用 `throw` 抛出异常，使用 `try/catch` 捕获异常，使用 `defer` 安排作用域清理。

## 抛出异常

```feng
func require_positive(value: int) {
  if value <= 0 {
    throw "value must be positive";
  }
}
```

可以抛出数值标量、`bool`、`string`、具名 enum、具名 tuple 和具体闭合用户类型，包括这些用户类型
支持的 `@abi` 和 `@value` 形式。具名 `catch` 使用相同的具体类型集合。array、任何 `spec` 视角值、
开放泛型、pointer、`void` 以及函数、Lambda、方法值等可调用值或类型，均不能用于抛出或具名 `catch`。

## 捕获异常

`try` 后跟一个表达式，不是语句块，并且必须至少包含一个 `catch`：

```feng
try load_config() catch error: string {
  println("load failed: {0}", error);
} catch {
  println("unknown failure");
}
```

多个 `catch` 按书写顺序匹配。具体类型分支应写在前面，匿名 `catch` 是兜底分支，必须放在最后。
`catch` 名称是不可重新赋值的头部绑定，后面的花括号是子块；子块可以声明同名局部绑定。每个
`catch` 子句的头部作用域彼此独立，因此不同子句可以复用同一异常名称。

匿名 `catch` 不绑定异常值，可以用 `throw;` 原样重抛：

```feng
try run_task() catch {
  throw;
}
```

`throw;` 所在的最近一层 `catch` 必须是当前函数、方法或 Lambda 内的匿名 `catch`。普通嵌套块
仍可重抛；嵌套的具名 `catch` 或新函数、方法、Lambda 不能沿用外层匿名 `catch` 的重抛权限。
`defer` 块内不能使用 `throw;`。

## try/catch 表达式

`try/catch` 可以产生值：

```feng
let port = try parse_port(text) catch error: string {
  8080;
};
```

`try` 主表达式的正常路径与每个正常结束的 `catch` 必须产生结果；返回当前函数、方法或 Lambda 的
`return` 路径以及逃逸当前 `try/catch` 的 `throw` 路径不产生结果。上下文提供目标类型时，每个正常
结果都必须能够贴合该类型；没有目标类型时，Feng 从这些正常结果确定目标类型，并要求其余正常结果
能够贴合。嵌套 Lambda 自己的 `return` 只返回该 Lambda。没有被任一 `catch` 匹配的异常会继续向
调用方传播。

## defer

`defer` 在离开当前词法作用域时执行，适合成对资源操作：

```feng
let file = File.create(path, FileMode.Read);
defer {
  file.close();
}

let content = file.readText();
```

同一作用域中的多个 `defer` 按后进先出顺序执行。正常离开、`return`、`break`、`continue` 和异常传播都会触发已经注册的清理。

## C 边界

异常不能跨越 C ABI 边界。ABI 函数必须在内部处理所有可能传播到边界的异常；C 函数的错误应通过返回值、错误码或回调约定表达。

## 精确匹配与清理边界

具名 catch 按精确具体类型匹配：enum 不等于底层整数，不同具名类型也不会因为布局相同而互相匹配。下面的完整程序还演示异常离开嵌套作用域时的 defer 顺序，以及清理函数自身抛错时的处理：

```feng
module manual_cleanup;
import std.io;

enum CodeA { Failure = 1 }

enum CodeB { Failure = 1 }

/** Throws one exact named enum type. */
func fail_exact() { throw CodeA.Failure; }

/** Registers cleanups in nested scopes before throwing. */
func work() {
  defer { println("outer"); }
  if true {
    defer { println("inner"); }
    throw "work failed";
  }
}

/** Models a cleanup operation that can fail. */
func close_resource() { throw "close failed"; }

/** Handles a possible close failure before returning to defer. */
func close_safely() {
  try close_resource() catch error: string { println(error); }
}

/** Calls a cleanup helper that handles its own exceptions. */
func cleanup_safely() {
  defer { close_safely(); }
}

/** Observes exact matching and cleanup order. */
func main(args: string[]) {
  try fail_exact() catch error: i32 {
    println("integer");
  } catch error: CodeB {
    println("other enum");
  } catch error: CodeA {
    println("exact");
  }
  try work() catch error: string { println("caught"); }
  cleanup_safely();
}
```

输出依次为 `exact`、`inner`、`outer`、`caught`、`close failed`。先离开内层作用域，再离开外层；每层已注册的 defer 按逆序执行，托管局部也按其正常退出规则释放。没有执行到的 defer 不会被登记。

`defer` 内任何位置都不能直接写 `return`、`throw` 或新的 `defer`；不能用 `break`／`continue` 跳向外层循环，但其内部新建循环可以使用自己的 break／continue。清理块调用的函数仍可能抛错；未在被调用函数内部处理的异常会中断该清理块的后续语句。当前实现对直接写在 defer 块中的 try/catch 存在异常捕获限制，请像上例一样在 `close_safely()` 内处理可恢复的清理失败，再由 defer 调用该辅助函数。

运行时 panic 会终止进程，不是可以由 `catch` 接住的 Feng 异常，也不能把它当作可靠执行全部 defer 的退出方式。资源持有与终结器的限制见[自定义类型](./user-defined-types.md)。
