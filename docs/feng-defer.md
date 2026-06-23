# Feng 语言 defer 规范

这个文档只可用于描述 feng 语言 defer 语法，开发文档请参考 [dev/feng-defer-dev.md](../dev/feng-defer-dev.md)

本文档用于补充 [feng-language.md](./feng-language.md) 中的 `defer` 概要说明,聚焦 Feng 语言中 `defer` 语句块的语法、执行时机与语句限制。托管局部在异常路径上的释放规则见 [Feng 语言异常模型规范](./feng-exception.md);`break` / `continue` / `return` / `throw` 的控制转移语义见 [Feng 语言流程控制规范](./feng-flow.md) 与 [Feng 语言异常模型规范](./feng-exception.md)。

## 1 职责

- `defer` 用于声明"当前块作用域结束时执行的清理动作",把资源申请与释放就近放置,降低遗漏释放的风险。
- `defer` 是块作用域的局部机制,只在声明它的块退出时生效,不跨函数传播,也不改变函数自身的签名或返回类型。
- `defer` 与托管局部的自动释放共享同一套清理基础设施,异常路径上的执行顺序见 [Feng 语言异常模型规范](./feng-exception.md)。

## 2 语法形式

`defer` 后必须紧跟一个以 `{}` 包裹的语句块,不可省略为单语句形式。

```feng
defer {
  // 清理语句
}
```

正确用法,在函数体内声明 `defer`:

```feng
func test() {
  let file = open("test.txt");
  defer { file.close(); }
  // do something
}
```

`defer` 仅可出现在函数体或函数体内嵌套的块作用域中,不可出现在模块顶层、`type` / `enum` / `spec` / `fit` 声明体中。

## 3 执行时机

- `defer` 在声明它的块作用域结束时执行,无论该块以何种方式退出。
- 正常退出:块内最后一条语句执行完毕后,该块的 `defer` 先于外层语句执行。
- 控制转移退出:`return` / `break` / `continue` 触发块退出时,该块的 `defer` 在控制转移生效之前执行。
- 异常路径退出:块内抛出异常或异常经此传播时,该块的 `defer` 在托管局部释放过程中一并执行,具体顺序见 [Feng 语言异常模型规范](./feng-exception.md)。
- 同一作用域内注册的多个 `defer` 按注册的逆序执行(LIFO)。
- `defer` 块内调用的外部函数抛出异常时,传播规则见 [Feng 语言异常模型规范](./feng-exception.md)。

## 4 defer 块内的语句限制

为避免清理动作改写当前块已确立的控制流或注册新的清理动作,`defer` 块内的语句存在以下限制:

- `defer` 块中任何位置都不能包含 `return` / `throw` / `defer` 语句,包括嵌套的子块内。
- `defer` 块中不能直接包含 `break` / `continue` 语句。
- `defer` 块内嵌套的 `for` / `while` 循环体中可以使用 `break` / `continue`,其作用域仅限该循环体,不作用于 `defer` 所在的外层块。

```feng
func work() {
  let file = open("test.txt");
  defer {
    // 合法:仅做清理
    file.close();
  }
  // do something
}

func bad() {
  defer {
    return 1;        // 非法:defer 块中不能使用 return
    throw "err";      // 非法:defer 块中不能使用 throw
    defer {}          // 非法:defer 块中不能再写 defer
    break;            // 非法:defer 块中不能直接使用 break
    continue;         // 非法:defer 块中不能直接使用 continue
  }
}

func bad_nested() {
  defer {
    if cond {
      return 1;       // 非法:嵌套子块内同样不能使用 return
      defer {}         // 非法:嵌套子块内同样不能再写 defer
    }
  }
}

func nested() {
  defer {
    for var i = 0; i < 10; i = i + 1 {
      if i == 5 { break; }  // 合法:break 作用于内层 for
    }
  }
}
```

## 5 与流程控制的关系

- `return` / `break` / `continue` 的语义本身不变,`defer` 仅在控制转移生效前追加一次清理执行。
- `defer` 不改变 `break` / `continue` 的作用对象;`break` / `continue` 始终作用于最近一层循环,见 [Feng 语言流程控制规范](./feng-flow.md)。
- `defer` 不可作为表达式使用,不参与 `if` / `match` / `try/catch` 表达式的块值提取。

## 6 与异常模型的关系

- `defer` 在异常路径上的执行与托管局部释放共享同一套清理基础设施,不引入额外的运行时机制。
- `defer` 块内禁止 `throw`,异常路径上仍可由块内调用的外部函数抛出异常,传播规则见 [Feng 语言异常模型规范](./feng-exception.md)。
- 异常不得穿越 C ABI 边界,`@abi` 顶层函数内的 `defer` 同样受 ABI 边界约束。

## 7 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范,提供资源清理概要并引用本文档。
- [feng-flow.md](./feng-flow.md): `return` / `break` / `continue` 的控制转移语义。
- [feng-exception.md](./feng-exception.md): 异常传播、托管局部在异常路径上的释放顺序与 C ABI 边界约束。
- 本文档: `defer` 语句块的语法、执行时机与语句限制。
