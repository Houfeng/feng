# 函数

函数用于组织行为，也可以通过可调用 `spec` 作为值传递。完整规则见[函数规范](../../../specifications/feng-function.md)和[变长参数规范](../../../specifications/feng-function-variadic.md)。

## 声明与调用

```feng
func add(a: int, b: int): int {
  return a + b;
}

let sum = add(20, 22);
```

所有参数都必须声明类型。无返回值函数可以省略 `: void`；未声明返回类型的普通函数也可以从一致的 `return` 路径推导返回类型。公共 API 建议显式声明返回类型。

函数可按名称和参数列表重载，返回类型不参与重载区分：

```feng
func describe(value: int): string {
  return "integer";
}

func describe(value: string): string {
  return value;
}
```

## 程序入口

可执行项目必须且只能有一个顶层入口：

```feng
func main(args: string[]) {
  // args[0] 是程序路径
}
```

入口返回类型固定为 `void`。库项目中的 `main` 只是普通函数，不会成为入口。

## 变长参数

变长参数写作 `T...`，并且必须位于参数列表最后：

```feng
import std.text;

func join_words(separator: string, words: string...): string {
  return string.join(separator, words);
}

let text = join_words(", ", "Feng", "is", "clear");
```

函数体内将变长参数按 `T[]` 使用。

## Lambda

Lambda 必须由可调用形式的 `spec` 提供目标类型：

```feng
spec Mapper(value: int): int;

let double: Mapper = (value: int) -> value * 2;
let transform: Mapper = (value: int) {
  let next = value + 1;
  return next * 2;
};
```

单表达式 Lambda 使用 `->`；多行 Lambda 直接使用块体。Lambda 可以捕获外层绑定，捕获的 `var` 与外层共享同一存储。

```feng
func make_adder(base: int): Mapper {
  return (value: int) -> base + value;
}
```

## 方法值

对象方法可以绑定为可调用值：

```feng
spec Action(): void;

type Button {
  func click() {
    println("clicked");
  }
}

let button = Button {};
let action: Action = button.click;
action();
```

方法值会保留原对象作为 `self`。若方法有重载，显式目标 `spec` 必须能够唯一确定所选重载。
