# C 互操作

Feng 通过显式 C ABI 声明调用原生库。互操作要求调用方准确描述外部签名、所有权与生命周期；完整规则以[C ABI 互操作规范](../../../specifications/feng-interop.md)为准。

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
