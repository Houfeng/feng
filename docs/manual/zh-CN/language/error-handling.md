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

可以抛出数值标量、`bool`、`string`、具名 enum、具名 tuple 和允许的具体闭合用户类型。不能抛出
array、`spec` 视角值、开放泛型、pointer、`void`、函数、Lambda 或方法值。具名 `catch` 使用相同的
类型集合。

## 捕获异常

`try` 后跟一个表达式，不是语句块，并且必须至少包含一个 `catch`：

```feng
try load_config() catch error: string {
  println("load failed: {0}", error);
} catch {
  println("unknown failure");
}
```

多个 `catch` 按书写顺序匹配。具体类型分支应写在前面，`catch error: unknown` 或匿名 `catch` 是兜底分支。
`catch` 名称是不可重新赋值的头部绑定，后面的花括号是子块；子块可以声明同名局部绑定。每个
`catch` 子句的头部作用域彼此独立，因此不同子句可以复用同一异常名称。

`unknown` 绑定只能重新抛出，不能访问字段或调用方法：

```feng
try run_task() catch error: unknown {
  throw error;
}
```

## try/catch 表达式

`try/catch` 可以产生值：

```feng
let port = try parse_port(text) catch error: string {
  8080;
};
```

正常路径与每个正常结束的 `catch` 必须产生结果；以 `throw` 结束的路径不产生结果。上下文提供目标
类型时，每个正常结果都必须能够贴合该类型；没有目标类型时，Feng 从这些结果确定目标类型，并要求
其余正常结果能够贴合。结果分支内不能使用返回外围函数的 `return`；应把最后一个表达式直接写成
该分支结果。嵌套 Lambda 自己的 `return` 不受此限制。没有被任一 `catch` 匹配的异常会继续向调用方
传播。

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
