# 类型

Feng 是静态类型语言。类型在编译期确定，已确定的不同类型之间进行类型转换时必须显式书写。
数值字面量可以贴合目标数值类型。通过已声明的满足关系使用对象契约，以及沿已声明的父子关系
自动投影到父契约，属于契约视角建立，不视为隐式类型转换。说明与示例见[契约与 fit](./contracts-and-fit.md)。

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
整数除法或取模的右操作数为 `0` 时属于未定义行为（UB）。对任一有符号整数类型，`MIN / -1` 与
`MIN % -1` 也属于未定义行为，其中 `MIN` 是该类型的最小值。Feng 不为这些情况增加运行时保护。

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

`Type[:length]` 创建指定长度、元素按零值初始化的数组。没有目标类型时结果为当前层只读的 `T[]`；
显式目标为 `T[!]` 时结果的当前层才可写。`length` 是只求值一次的整数，有效范围为 `0` 到目标平台
`int` 类型的最大值：编译期可确定的范围错误直接编译失败，动态范围错误在分配前触发 panic，不会
截断或回绕。有效长度对应的分配大小不可表示或内存不足时也触发 panic。元素类型必须静态具备有限
默认零值，包括常量零长度和动态长度；数组自身的空数组默认值与由显式元素组成的字面量不受此限制。

数组长度创建后不变，Feng 不设置固定的数组嵌套层数上限。索引必须位于 `0` 到 `length - 1`，负数或
越界索引在读写前触发 panic。需要可增长集合时使用 `std.collections.List<T>`。

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

具名元组是值类型，元素始终不可变。`var` 绑定可以替换整个元组，但不能原地修改某个元素。新的 tuple
字面量必须贴合具名目标类型；已有 tuple 值可以按值复制，无初始化器的 tuple 绑定使用各元素的默认
零值。tuple 不是对象类型，没有普通构造函数，也不能写成 `Pair()`、`Pair { ... }` 或
`Pair() { ... }`。

## 枚举

枚举形成独立的具名类型：

```feng
enum Status {
  Pending,
  Running,
  Done
}

let status = Status.Running;
let raw = (i32)status;
```

enum 的底层表示固定为 `i32`，不随平台 `int` 的宽度变化。枚举可以全部使用隐式递增值，也可以全部
显式指定 `-2147483648...2147483647` 范围内的整数字面量；两种方式不能混用。允许 `enum` 显式转换
到任意整数类型，不允许任何整数类型显式转换到 `enum`，也不允许二者之间发生隐式转换。

## 用户定义类型与契约类型

对象类型、`spec` 契约、联合类型和泛型分别在[自定义类型](./user-defined-types.md)、[契约与 fit](./contracts-and-fit.md)、[模式匹配](./pattern-matching.md)和[泛型](./generics.md)中展开。

## 数值字面量与显式转换

整数支持十进制、`0x` 十六进制、`0b` 二进制和 `0o` 八进制；数字之间可以用 `_` 分隔。浮点数支持 `e` 指数。没有 `L`、`U`、`F` 等数值后缀，指定类型时使用类型标注。

```feng
module manual_numeric;
import std.io;
import std.numeric;

/** Shows literal notation, conversions and grouping. */
func main(args: string[]) {
  let binary = 0b1111_1111;
  let octal = 0o377;
  let hex = 0xFF;
  let million = 1_000_000;
  let scientific = 1.5e3;
  let fraction = 2.0e-1;
  println<int>("{0} {1} {2} {3}", binary, octal, hex, million);
  println<int>("{0}", (int)scientific);
  let wide: u16 = 300;
  let low = (u8)wide;
  let truncated = (i32)-3.9;
  println<int>("{0} {1}", (int)low, (int)truncated);
  let precise: i32 = 16_777_217;
  let rounded = (f32)precise;
  println<i32>("{0}", (i32)rounded);
  println<int>("{0} {1}", 2 + 3 * 4, (2 + 3) * 4);
  let small: i32 = 7;
  let large: i64 = 7;
  if (i64)small == large { println("same"); }
}
```

输出依次为 `255 255 255 1000000`、`1500`、`44 -3`、`16777216`、`14 20`、`same`。整数缩窄会丢弃高位，浮点转整数去掉小数部分；整数转浮点或浮点缩窄可能损失精度，显式写出转换并不保证无损。数字分隔符不能位于字面量首尾或紧跟进制前缀。

`bool` 不属于数值类型，不能用数值与 bool 互转来表达条件；应写 `value != 0` 等布尔表达式。比较不同的已确定数值类型时，先像上例一样显式转换到选定的共同类型。其他具名类型也不能因形状相同而直接互相比较；先使用该类型支持的显式转换，再比较同类型的值。

## 运算符优先级

下表从高到低排列；同一行中，一元运算从右向左结合，其他列出的运算从左向右结合。复杂表达式可以用圆括号明确分组。

| 优先级 | 形式 |
| --- | --- |
| 1 | 调用 `f(...)`、成员 `value.member`、下标 `value[index]` |
| 2 | `~x`、`-x`、`!x` |
| 3 | `*`、`/`、`%` |
| 4 | `+`、`-` |
| 5 | `<<`、`>>` |
| 6 | `&` |
| 7 | `^` |
| 8 | `\|` |
| 9 | `<`、`<=`、`>`、`>=` |
| 10 | `==`、`!=` |
| 11 | `&&` |
| 12 | `\|\|` |

赋值是语句，不作为表达式参与该表；Feng 不支持 `++`、`--` 或逗号表达式。`&&` 与 `||` 短路求值，右侧只在需要时执行。

## 数组每一层的写权限

数组后缀从内向外阅读。最右侧后缀控制外层数组是否能替换某一行，前一个后缀控制行内是否能替换元素；`let` 只限制绑定本身。

```feng
module manual_array_layers;
import std.io;
import std.numeric;

/** Modifies independent layers through their declared permissions. */
func main(args: string[]) {
  let rows: int[!][] = [[1, 2], [3, 4]];
  rows[0][0] = 9;
  let slots: int[][!] = [[1, 2], [3, 4]];
  slots[0] = [8, 9];
  let both: int[!][!] = [[1, 2]];
  let readonly_outer = (int[!][])both;
  readonly_outer[0][0] = 7;
  println<int>("{0} {1} {2}", rows[0][0], slots[0][0], both[0][0]);
}
```

输出为 `9 8 7`。转换没有复制数组，两个视角仍共享数据。

| 类型 | 替换一行 `a[0] = ...` | 替换行内元素 `a[0][0] = ...` |
| --- | --- | --- |
| `int[][]` | 不允许 | 不允许 |
| `int[!][]` | 不允许 | 允许 |
| `int[][!]` | 允许 | 不允许 |
| `int[!][!]` | 允许 | 允许 |

只能显式移除已有写权限，不能通过转换取得更强的写权限。元素若是普通对象，数组元素只读并不冻结对象本身；对象的 `var` 字段仍按其访问权限使用。

## 空元组、具名转换与枚举行为

空元组用 `type Unit();` 声明，并用 `()` 在目标类型下创建。不同具名元组只有在元素数量和各位置类型完全相同时才能显式转换。tuple 和 enum 都可以通过 fit 添加方法：

```feng
module manual_tuple_enum;
import std.io;
import std.numeric;

/** An empty named tuple. */
type Unit();

/** Two distinct named tuples with identical element types. */
type Position(int, int);

type Dimensions(int, int);

/** Adds behavior without adding tuple elements. */
fit Position {
  /** Adds the two coordinates. */
  func total(): int { return self.item1 + self.item2; }
}

/** Uses an explicit value for every member. */
enum Status { Pending = 7, Done = 12 }

/** Adds behavior to the named enum. */
fit Status {
  /** Describes the current enum value. */
  func label(): string {
    if self == Status.Done { return "done"; }
    return "pending";
  }
}

/** Demonstrates construction, conversion and the first-member default. */
func main(args: string[]) {
  let unit: Unit = ();
  let default_unit: Unit;
  let position: Position = (2, 3);
  let dimensions = (Dimensions)position;
  let pending: Status;
  println<int>("{0} {1} {2}", position.total(), dimensions.item1, (int)pending);
  println(Status.Done.label());
}
```

输出为 `5 2 7`、`done`。`Status` 的默认值是首项 `Pending`，即使其底层值不是零；显式枚举值必须是范围内的整数字面量，不能混入自动递增项。相同元素形状不会合并 `Position` 与 `Dimensions` 的类型身份，也不会共享各自的扩展方法。

tuple 支持 0 个或 2～8 个元素，不支持单元素 tuple；`(value)` 是分组表达式。空元组同样没有 `Unit()` 构造函数。更多 fit 用法见[契约与 fit](./contracts-and-fit.md)。
