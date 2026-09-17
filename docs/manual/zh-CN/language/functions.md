# 函数

函数用于组织行为，也可以通过可调用 `spec` 作为值传递。

## 声明与调用

```feng
func add(a: int, b: int): int {
  return a + b;
}

let sum = add(20, 22);
```

所有参数都必须声明类型。无返回值函数可以省略 `: void`；未声明返回类型的普通函数也可以从一致的 `return` 路径推导返回类型。公共 API 建议显式声明返回类型。

显式声明或推导为非 `void` 的函数，不能有正常到达函数末尾却没有返回值的路径：

```feng
func choose(flag: bool): i32 {
  if flag {
    return 1;
  }
  // 错误：flag 为 false 时会正常到达函数末尾。
}
```

编译器会在编译期拒绝这个函数。完整 `if / else` 的所有分支都返回值时合法；逃逸函数的 `throw` 会
终止当前路径，不要求随后返回；若异常被函数内的 `catch` 捕获，则继续以该 `catch` 的执行结果判断。
可能零次执行或通过 `break` 退出的循环不能保证返回；循环条件写为字面量 `true` 且没有作用于该循环
的可达 `break` 时，循环后的代码不可达。永不退出的 `while true {}` 可以出现在非 `void` 函数中，
因为它不会正常到达函数末尾。

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

## 转发变长参数

已有数组作为整个变参数组传递时写 `...items`。下面同时演示普通打包、预打包转发和变参可调用契约：

```feng
module manual_variadic;
import std.io;
import std.numeric;

spec Sum(values: int...): int;

/** Adds an ordinary variadic argument array. */
func sum(values: int...): int {
  var total = 0;
  for let value in values { total += value; }
  return total;
}

/** Forwards the existing array without packing another one. */
func forward(values: int...): int { return sum(...values); }

/** Uses direct calls and a variadic callable value. */
func main(args: string[]) {
  let items: int[] = [1, 2, 3];
  let writable: int[!] = [7, 8];
  let readonly = (int[])writable;
  let operation: Sum = sum;
  println<int>("{0} {1} {2}", sum(...items), forward(4, 5), operation(...readonly));
}
```

输出为 `6 9 15`。`...items` 必须是最后一个实参，且恰好占据整个变参部分；前面只能有固定参数，不能写成 `sum(1, ...items)`。它不是通用数组展开操作。

转发目标必须有变参声明，数组必须匹配其只读 `T[]` 形状。`T[!]` 不能直接转发；上例先显式移除写权限。转发保留同一个数组，不复制或重新打包元素。可调用契约的 `T...` 与普通 `T[]` 参数不是同一签名，绑定或显式转换时也不能互换。

## 函数值的来源、转换与零值

可调用值可以来自顶层函数、具体类型的静态／实例方法和对象契约视角的方法。泛型函数或方法形成值时，必须先写完整类型实参：

```feng
module manual_callable;
import std.io;
import std.numeric;

spec Calculate(value: int): int;

spec OtherCalculate(value: int): int;

spec Read(): int;

spec Readable { func read(): int; }

/** Supplies instance and static callable sources. */
type Reader: Readable {
  let value: int;

  /** Reads the retained receiver. */
  func read(): int { return self.value; }

  /** Doubles a value without capturing an instance. */
  static func twice(value: int): int { return value * 2; }
}

/** Supplies a top-level callable source. */
func increment(value: int): int { return value + 1; }

/** Must be explicitly closed before it becomes a callable value. */
func identity<T>(value: T): T { return value; }

/** Exercises each source and a default callable. */
func main(args: string[]) {
  let top: Calculate = increment;
  let generic: Calculate = identity<int>;
  let method: Calculate = Reader.twice;
  let receiver: Readable = Reader { value: 7 };
  let read: Read = receiver.read;
  let converted = (OtherCalculate)method;
  let empty: Calculate;
  println<int>("{0} {1} {2} {3} {4}", top(1), generic(3), read(), converted(4), empty(9));
}
```

输出为 `2 3 7 8 0`。不同 callable spec 即使签名完全相同，也不能直接互相赋值；完整参数顺序、类型、变参形态与返回类型一致时可显式转换。可调用零值可安全调用：无返回值时不做任何事，有返回值时产生返回类型默认值。

泛型来源应写 `identity<int>`、`object.method<int>` 或 `Type.method<int>`；目标 callable 不会反向推导来源的泛参。泛型 owner 也必须闭合，例如 `Box<int>.method<string>`。存在重载时，目标 callable 的签名必须能够唯一选定来源。

实例方法值保留形成点的接收者；值类型会复制接收者，引用类型保留同一对象。示例与连续调用的状态变化见[自定义类型](./user-defined-types.md)。
