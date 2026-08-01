# 类型

Feng 是静态类型语言。类型在编译期确定，跨类型转换必须显式书写。权威规则见[内建类型规范](../../../specifications/feng-builtin-type.md)、[类型规范](../../../specifications/feng-type.md)和[元组规范](../../../specifications/feng-tuple.md)。

## 内建标量类型

| 类别 | 类型 | 常用别名 |
| --- | --- | --- |
| 有符号整数 | `i8`、`i16`、`i32`、`i64` | `int` |
| 无符号整数 | `u8`、`u16`、`u32`、`u64` | `byte`、`uint` |
| 浮点数 | `f32`、`f64` | `float`、`double` |
| 布尔 | `bool` | — |
| 字符串 | `string` | — |

`int` 和 `uint` 随平台位宽变化：32 位平台对应 32 位类型，64 位平台对应 64 位类型。整数字面量默认推导为 `int`，浮点字面量默认推导为 `double`。

```feng
let count: i32 = 42;
let size: uint = (uint)count;
let ratio: f64 = 0.5;
let ready = true;
```

运行时整数溢出按位宽回绕；编译期可确定的越界字面量会被拒绝。

## 字符串

`string` 是不可变的 UTF-8 字符串，默认值为 `""`。双引号字符串支持 `\\`、`\"`、`\n`、`\r`、`\t`、`\0` 与十六进制字节转义；反引号字符串保留其中的原始内容。

```feng
let line = "first\nsecond";
let path = `C:\data\feng`;
let message = "Hello, " + "Feng";
```

导入 `std.text` 后可使用 `length()`、搜索、切分、大小写转换等方法。字符串长度按 UTF-8 字节数计算；需要 Unicode 字符或字素簇视图时使用标准库的 rune 或 grapheme API。

## 数组

`T[]` 是当前层元素只读的固定长度数组，`T[!]` 是当前层元素可写的固定长度数组。数组是托管引用类型：赋值复制引用，不复制元素。

```feng
let values: int[] = [1, 2, 3];
let buffer: byte[!] = byte[:1024];
let matrix: int[!][!] = [[1, 2], [3, 4]];

buffer[0] = (byte)65;
matrix[0][1] = 9;
```

`Type[:length]` 创建指定长度的可写数组。数组长度创建后不变；需要可增长集合时使用 `std.collections.List<T>`。

只允许通过显式转换移除写权限：

```feng
let writable: int[!] = [1, 2, 3];
let readonly = (int[])writable;
```

## 具名元组

Feng 没有匿名元组类型。用圆括号形式声明具名元组：

```feng
type Point(f64, f64);
type Pair<T, U>(T, U);

let origin: Point = (0.0, 0.0);
let item: Pair<int, string> = (1, "one");
println("{0}", origin.item1);
```

具名元组是值类型，元素始终不可变。`var` 绑定可以替换整个元组，但不能原地修改某个元素。

## 枚举

枚举形成独立的具名类型：

```feng
enum Status {
  Pending,
  Running,
  Done
}

let status = Status.Running;
let raw = (int)status;
```

枚举可以全部使用隐式递增值，也可以全部显式指定整数字面量；两种方式不能混用。允许 `enum` 到 `int` 的显式转换，不允许 `int` 到 `enum` 的转换。

## 用户定义类型与契约类型

对象类型、`spec` 契约、联合类型和泛型分别在[自定义类型](./user-defined-types.md)、[契约与 fit](./contracts-and-fit.md)、[模式匹配](./pattern-matching.md)和[泛型](./generics.md)中展开。
