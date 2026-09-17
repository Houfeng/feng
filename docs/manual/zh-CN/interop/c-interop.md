# C 互操作

Feng 通过显式 C ABI 声明调用原生库。互操作要求调用方准确描述外部签名、所有权与生命周期。

## 导入 C 函数

`extern func` 声明外部函数，调用约定注解声明库名和可选的 C 符号名：

```feng
@cdecl("m")
extern func sin(value: f64): f64;

@cdecl("m", "fabs")
extern func absolute(value: f64): f64;
```

还可使用 `@stdcall` 或 `@fastcall`。每个 C ABI `extern func` 必须且只能带一个有参数的调用约定注解。

库名由系统链接规则补全前后缀。需要不同 Feng 名称时使用第二个参数指定真实 C 符号。

## ABI 类型

标量和枚举可以直接按值传递。结构体式 payload 使用 `@abi type`：

```feng
@abi
type Point {
  var x: i32;
  var y: i32;
}

@cdecl("geometry")
extern func point_distance(left: Point, right: Point): f64;
```

`@abi type` 的直接字段只能使用规范允许的标量、枚举和指针形态。不能把普通 Feng 对象、字符串、数组或泛型实例直接内联为 ABI 字段。

## 指针

指针类型写作 `T*`，只用于 ABI 存储和传递：

```feng
@cdecl("libc", "strlen")
extern func c_strlen(value: string*): uint;

let text = "Feng";
let length = c_strlen(&text);
```

Feng 指针不能直接解引用、算术运算、跨类型转换或调用。只允许同类型指针进行 `==` / `!=` 比较。

数据指针默认只在调用期间有效。若 C 侧缓存指针、异步使用或让指针逃逸，Feng 代码必须保活原 owner，并遵循外部 API 的所有权约定。

## 数组与长度

ABI 兼容数组取址后只传递首元素地址，不会隐式传递长度：

```feng
import std.collections;

@cdecl("checksum")
extern func checksum(data: byte*, length: uint): u32;

let bytes: byte[] = [1, 2, 3];
let value = checksum(&bytes, (uint)bytes.length());
```

长度必须通过独立参数或 ABI 字段明确传递。可写性由数组的 `T[]` / `T[!]` 层级决定。

## 回调

用 `@abi spec` 声明函数指针签名，用顶层 `@abi func` 提供 Feng 回调：

```feng
@abi
spec Compare(left: i32, right: i32): i32;

@abi
func compare_int(left: i32, right: i32): i32 {
  return left - right;
}

let callback: Compare* = &compare_int;
```

只有顶层 `@abi func` 可以取为函数指针；普通函数、方法、Lambda 和闭包都不可以。

## 异常与资源

Feng 异常不能穿越 C ABI 边界。ABI 函数必须捕获内部异常，并转换为 C 能理解的返回值或错误码。

`@abi` 只描述布局与调用兼容性，不表达资源所有权。谁分配、谁释放、指针可用多久，都必须依据具体 C API 契约在 Feng 包装层中明确处理。

## 完整示例：打包并使用 C 静态库

下面的项目把 C 静态库与 Feng 绑定一起打包，再由应用引用。它只依赖本机 C 工具链和 Feng，不依赖标准库。按以下路径保存文件：

```text
geometry-demo/
├── geometry/
│   ├── feng.fm
│   ├── native/geometry.c
│   └── src/bindings.ff
└── app/
    ├── feng.fm
    └── src/main.ff
```

`geometry/native/geometry.c`:

```text
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/** Payload layout shared with the Feng declaration. */
typedef struct Point { int32_t x; int32_t y; } Point;

/** Private C resource layout, opaque to Feng. */
typedef struct Counter { int32_t value; } Counter;

/** Changes a copy and returns its payload by value. */
Point point_shift_value(Point point) {
    point.x += 10;
    return point;
}

/** Changes the borrowed payload in place. */
void point_shift_pointer(Point *point) { point->x += 20; }

/** Allocates a resource; the caller must destroy a non-null result. */
Counter *counter_create(int32_t initial) {
    Counter *counter = malloc(sizeof(*counter));
    if (counter != NULL) counter->value = initial;
    return counter;
}

/** Reads a live resource. */
int32_t counter_read(Counter *counter) { return counter->value; }

/** Releases a resource exactly once. */
void counter_destroy(Counter *counter) { free(counter); }

/** Prints one value without a Feng library dependency. */
void show_number(int32_t value) { printf("%d\n", (int)value); }

/** Has the same C signature for every Feng type argument. */
int32_t geometry_version(void) { return 1; }
```

`geometry/src/bindings.ff`:

```feng
open module geometry;

/** Matches the C Point payload in field order and width. */
@abi
open type Point { var x: i32; var y: i32; }

/** Names the opaque C resource; only Counter* crosses the boundary. */
@abi
open type Counter {}

/** Calls the by-value C operation. */
@cdecl("geometry")
extern func point_shift_value(point: Point): Point;

/** Calls the borrowed-pointer C operation. */
@cdecl("geometry")
extern func point_shift_pointer(point: Point*): void;

/** Creates a resource owned by the caller. */
@cdecl("geometry")
open extern func counter_create(initial: i32): Counter*;

/** Reads a live resource. */
@cdecl("geometry")
open extern func counter_read(counter: Counter*): i32;

/** Releases a previously created resource. */
@cdecl("geometry")
open extern func counter_destroy(counter: Counter*): void;

/** Prints through the C library. */
@cdecl("geometry")
open extern func show_number(value: i32): void;

/** Demonstrates a generic declaration with a fixed C signature. */
@cdecl("geometry", "geometry_version")
open extern func abi_version<T>(): i32;

/** Keeps the by-value C boundary inside the binding package. */
open func shift_value(point: Point): Point { return point_shift_value(point); }

/** Borrows the payload while this Feng call keeps its owner alive. */
open func shift_in_place(point: Point) { point_shift_pointer(&point); }
```

`geometry/feng.fm`:

```text
[package]
name: "geometry"
version: "0.1.0"
target: "lib"
src: "src/"
out: "build/"

[assets]
extlib: "extlib/"
```

`app/src/main.ff`:

```feng
module geometry_demo;
import geometry;

/** Contrasts Feng references, C value copies and borrowed pointers. */
func main(args: string[]) {
  let point = Point { x: 1, y: 2 };
  let alias = point;
  alias.x = 3;
  show_number(point.x);
  let shifted = shift_value(point);
  show_number(point.x);
  show_number(shifted.x);
  shift_in_place(point);
  show_number(point.x);
  show_number(alias.x);

  let handle = counter_create(7);
  let null_handle: Counter*;
  if handle == null_handle { show_number(-1); return; }
  defer { counter_destroy(handle); }
  show_number(counter_read(handle));
  show_number(abi_version<Point>());
  show_number(abi_version<Counter>());
}
```

`app/feng.fm`:

```text
[package]
name: "geometry_demo"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"

[dependencies]
geometry: "../geometry/build/pkg/geometry-0.1.0.fb"
```

在 `geometry-demo/` 目录执行以下命令。这里以 macOS ARM64 为例，`cc` 和 `ar` 必须面向相同目标；Linux 主机应把两处目录名和 `--platform` 一并换成对应的 Feng 平台标识，例如 `linux-x64-gnu`，并使用该目标的 C 工具链。

```bash
mkdir -p geometry/build/native geometry/extlib/macos-arm64
cc -std=c11 -c geometry/native/geometry.c -o geometry/build/native/geometry.o
ar rcs geometry/extlib/macos-arm64/libgeometry.a geometry/build/native/geometry.o
feng pack geometry --platform=macos-arm64
feng run app
```

正常输出依次为 `3`、`3`、`13`、`23`、`23`、`7`、`1`、`1`，每个数字一行。

`@abi` 不改变 Feng 内部普通对象的引用语义：`alias` 与 `point` 指向同一个对象。传入 `Point` 的 C 函数接收字段 payload 的副本，返回的 payload 在 Feng 中形成新对象；传入 `Point*` 的函数则借用原对象 payload 的地址，所以修改也能从 `alias` 观察到。这里 C 不保存借入的地址，调用期间 Feng 持有 `point`。

当前版本中，跨包直接调用以有字段 `@abi` 类型传值或借址的 extern 存在代码生成限制。上例将这两类 C 调用放在声明 `Point` 的绑定包内，对外提供普通 Feng 函数 `shift_value` 和 `shift_in_place`；应用按 Feng 对象语义使用它们。

无字段的 `@abi type Counter` 只为不透明指针提供类型名称，不能按值进入 ABI，也不能用 `&` 对该空对象生成 C 资源。资源由 C 创建，Feng 只保存 `Counter*`，通过 C API 操作；示例先检查空指针，再登记一次释放。资源释放后不可再使用该指针或其副本。生命周期说明见[自定义类型](../language/user-defined-types.md)。

## 原生库配置

`[assets].extlib` 指向包含平台子目录的原生库根目录。打包后，`libgeometry.a` 位于 `.fb` 的 `extlib/<platform>/`；应用导入的 `@cdecl("geometry")` 声明要求该库，编译器据此从依赖包提取并链接它。只放置库文件而没有对应 extern 库声明，不会自动加入链接。

没有随包提供的裸库名按系统链接库规则查找。`--lib` 是直接编译时补充纯原生依赖的参数；通常应通过 extern 声明和 `.fb` 携带原生依赖。它不会让一个尚未打包的源码项目目录成为原生库搜索路径。项目与包的构建方式见[构建与运行](../projects/build-and-run.md)及[依赖管理](../projects/dependencies.md)。

## 受限泛型 extern

上例的 `abi_version<T>()` 展示受限泛型 extern：无论 `T` 是 `Point` 还是 `Counter`，外部函数始终是无参、返回 `i32` 的 `geometry_version`，类型参数不改变 C 签名，也不会使 C 函数成为模板。这里没有可用于推导 `T` 的实参，调用必须显式给出类型参数。

不要把它扩展成 `extern func identity<T>(value: T): T` 或包含开放 `T*` 的 C 声明。每个参数和返回位置都必须在声明处具有唯一、固定且通过 ABI 检查的外部表示，不能等到某个调用实例才判断。需要随类型变化的 Feng 处理时，在普通泛型 Feng 包装函数内调用固定签名的 extern。
