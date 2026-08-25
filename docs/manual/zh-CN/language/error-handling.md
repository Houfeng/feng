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

可以抛出标量、`bool`、`string`、数组或具体用户类型。不能抛出函数、方法值或 `spec` 视角值。

## 捕获异常

`try` 后跟一个表达式，不是语句块：

```feng
try load_config() catch error: string {
  println("load failed: {0}", error);
} catch {
  println("unknown failure");
};
```

多个 `catch` 按书写顺序匹配。具体类型分支应写在前面，`catch error: unknown` 或匿名 `catch` 是兜底分支。

`unknown` 绑定只能重新抛出，不能访问字段或调用方法：

```feng
try run_task() catch error: unknown {
  throw error;
};
```

## try/catch 表达式

`try/catch` 可以产生值：

```feng
let port = try parse_port(text) catch error: string {
  8080;
};
```

正常路径与每个正常结束的 `catch` 必须产生结果，其目标类型选择与贴合遵循[流程控制规范的统一分支规则](../../../specifications/feng-flow.md#41-if--match--try-%E8%A1%A8%E8%BE%BE%E5%BC%8F%E7%BB%93%E6%9E%9C%E7%B1%BB%E5%9E%8B)。省略 `catch` 表示只建立异常传播点，异常会继续向调用方传播。

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
