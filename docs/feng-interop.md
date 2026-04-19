# Feng 语言 C 互操作规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的 C 互操作概要说明,聚焦 Feng 语言与 C 库来源声明、C 兼容类型、外部函数声明、导出函数和回调函数定义规则。

## 1 C互操作概览

- Feng 通过调用方式注解、`extern type` 和 `extern fn` 实现与 C 语言的安全互操作。
- 对无函数体的 `extern fn` 声明,使用带一个库参数的 `@cdecl`、`@stdcall` 或 `@fastcall` 指定 C 库来源与调用方式。
- `extern type` 用于定义 C 兼容结构体、联合体和 C 函数指针类型。
- `extern fn` 用于声明 C 外部函数、定义可传给 C 的 Feng 回调函数以及定义 C ABI 兼容导出函数。
- 支持使用 `@union` 将对象形式的 `extern type` 显式声明为 C 联合体。
- 支持使用无参数的 `@cdecl`、`@stdcall` 和 `@fastcall` 三个内建注解显式声明带函数体的 `extern fn` 定义的 C 函数调用方式; 未显式标注时默认使用 `cdecl`。

## 2 C库来源与调用方式注解

使用 `@cdecl("库名/路径")`、`@stdcall("库名/路径")` 或 `@fastcall("库名/路径")` 为无函数体的 `extern fn` 声明同时指定 C 库来源与调用方式。这三个注解在导入场景下的唯一参数支持以下两种写法:

1. 直接书写字符串字面量
2. 引用当前 `mod` 作用域中以字符串字面量初始化的 `let` 绑定

无论采用哪种写法,编译器最终都会在编译期把第一个参数解析为以下三种库来源之一:

1. 系统库名: 无特殊路径前缀,编译器自动补全系统库前缀和后缀
2. 相对路径: 以 `./` 或 `../` 开头,相对于当前 `.f` 文件路径
3. 绝对路径: 以 `/` 开头,直接指定库文件完整路径

补充规则:

- `@cdecl("...")`、`@stdcall("...")`、`@fastcall("...")` 仅适用于无函数体的 `extern fn` 声明,不适用于 `extern type`、普通 `fn`、带函数体的 `extern fn` 回调定义或 C ABI 导出函数定义。
- 带参数的调用方式注解必须且只能带一个参数,该参数表示库名或路径。
- 若该参数使用模块级 `let` 绑定,则该绑定必须在当前 `mod` 作用域内对该声明可见,且初始化值必须直接是字符串字面量,不能是计算表达式或 `var` 绑定。
- 不同 `extern fn` 声明在同一文件或同一 `mod` 中可以指向不同原生库,不再要求“一个文件只归属于一个 C ABI 库”。
- 无函数体的 `extern fn` 声明必须且只能使用一个带参数的调用方式注解; 调用方式由注解名本身唯一确定。

```feng
let math_lib = "m";
let local_lib = "./libtest.so";

@cdecl(math_lib)
extern fn sin(x: float): float;

@stdcall(local_lib)
extern fn create_point(x: int, y: int): Point;

@cdecl("/usr/local/lib/libcurl.so")
extern fn curl_global_init(flags: u64): int;
```

书写顺序示例:

```feng
pu mod libc.math;

use libc.base;
use libc.extra;

let math_lib = "m";

@cdecl(math_lib)
extern fn sin(x: float): float;
```

## 3 C兼容结构体与联合体定义(extern type)

使用对象形式的 `extern type` 可定义可与 C 直接映射的结构体或联合体,无需 C 侧额外声明。未显式标注时,`extern type Name { ... }` 默认按 C 结构体处理; 若需声明为 C 联合体,可在 `extern type` 上一行添加 `@union` 注解。其内存布局、字节序和内存对齐与 C 语言保持一致,可直接在 feng 与 C 之间按值或按指针传递。

语法规则:

- 仅允许声明成员变量,禁止添加构造函数、成员方法
- 成员仅支持: 基础类型、其他 `extern type` 类型、固定长度数组
- 成员需用 `var` 或 `let` 修饰,遵循 feng 变量声明规范
- `@union` 仅适用于对象形式的 `extern type`,不适用于函数指针形式的 `extern type`
- 在对象形式的 `extern type` 上,`@union` 通常单独写在声明的上一行
- 未显式标注 `@union` 时,对象形式的 `extern type` 默认按 C 结构体处理
- 联合体的所有成员共享同一块内存,其有效成员语义与 C 联合体保持一致
- Feng 的 GC 不管理该类型内存,需通过 C 函数或手动释放
- 编译器自动校验与 C 的内存兼容性

定义示例:

```feng
// 定义与 C 完全兼容的 Point 结构体
extern type Point {
    var x: int;
    var y: int;
}

// 嵌套 C 兼容结构体
extern type Rect {
    var p1: Point;
    var p2: Point;
    var area: float;
}

// 定义与 C 完全兼容的联合体
@union
extern type IntOrFloat {
    var i: int;
    var f: float;
}
```

## 4 C函数指针类型定义(extern type)

使用 `extern type` 定义与 C 函数指针完全兼容的函数类型,用于 C 回调函数传递,签名需与 C 侧完全一致。函数指针类型本身不使用 `@union` 或调用方式注解,调用方式由与之匹配的无函数体 `extern fn` 声明上的带参调用方式注解,或带函数体 `extern fn` 定义上的无参调用方式注解负责标注。

```feng
// 定义 C 兼容的比较函数指针类型
extern type CmpFunc(a: int, b: int): int;

// 定义 C 兼容的结构体回调函数类型
extern type PointCallback(p: Point);
```

## 5 Feng实现的 `extern fn` 调用方式注解

Feng 提供 `@cdecl`、`@stdcall` 和 `@fastcall` 三个内建注解,用于显式声明带函数体的 `extern fn` 定义的 C ABI 调用方式。它们适用于 Feng 回调函数定义和 C ABI 导出函数定义; 在这类场景中,注解不得带参数。

规则说明:

- `@cdecl`: 无参数时声明使用 C 默认调用方式。
- `@stdcall`: 无参数时声明使用 `stdcall` 调用方式。
- `@fastcall`: 无参数时声明使用 `fastcall` 调用方式。
- 每个带函数体的 `extern fn` 定义最多只能使用一个调用方式注解。
- 带函数体的 `extern fn` 定义若使用调用方式注解,则该注解不得带参数。
- 调用方式注解仅适用于 `extern fn`,不适用于 `extern type` 声明。
- 在带函数体的 `extern fn` 上,调用方式注解通常单独写在定义的上一行,以保持书写清晰。
- 未显式标注时,带函数体的 `extern fn` 定义默认按 `cdecl` 处理。

调用方式示例:

```feng
extern fn my_int_cmp(a: int, b: int): int {
    return a - b;
}

@stdcall
pu extern fn create_point_export(x: int, y: int): Point {
    return Point { x: x, y: y };
}
```

## 6 C外部函数声明(extern fn)

使用无函数体的 `extern fn` 声明 C 语言实现的外部函数,用于调用 C 库函数。参数和返回值需为 C 兼容类型,如基础类型、`extern type` 类型和指针。每个这类声明都必须在上一行使用带一个库参数的调用方式注解指定库来源与调用方式。

规则说明:

- 无函数体的 `extern fn` 声明必须使用且只能使用一个带参数的 `@cdecl`、`@stdcall` 或 `@fastcall`。
- 带参数注解的参数可以是字符串字面量,也可以是当前 `mod` 作用域中以字符串字面量初始化的 `let` 绑定。
- 带参数注解的注解名本身决定调用方式,不再额外传入字符串形式的调用约定。
- 无函数体的 `extern fn` 声明不得再叠加其他调用方式注解。

```feng
let math_lib = "m";

@cdecl(math_lib)
extern fn sin(x: float): float;

@cdecl(math_lib)
extern fn sqrt(x: float): float;

@stdcall("./libpoint.so")
extern fn create_point(x: int, y: int): Point;

@cdecl("./libpoint.so")
extern fn set_point_callback(cb: PointCallback);
```

## 7 Feng 回调函数定义(extern fn)

使用带函数体的 `extern fn` 定义可作为函数指针传给 C 的 Feng 回调函数。此类函数由 Feng 实现,编译器自动生成 C 兼容 ABI,并进行强制编译期检查。若需显式指定非默认调用方式,可在定义前添加第 5 节中的无参数内建注解; 未显式指定时默认使用 `cdecl`。

约束规则:

- 禁止捕获外部变量
- 禁止使用闭包
- 仅支持 C 兼容类型作为参数和返回值
- 不可使用 feng GC 托管类型,如 `string`、动态数组、原生 `type` 对象
- 调用方式必须与目标回调签名对应的 C 侧声明保持一致

```feng
extern fn my_int_cmp(a: int, b: int): int {
    return a - b;
}

extern fn my_point_handle(p: Point) {
    print(p.x, p.y);
}
```

## 8 C ABI导出函数

使用带函数体的 `pu extern fn` 可定义对外导出的 C ABI 函数。此类函数会生成可被 C 或其他兼容 C ABI 的语言直接调用的导出符号。

规则说明:

- 仅顶层 `pu extern fn` 会作为公开 C ABI 符号导出。
- 导出函数的参数与返回值必须全部为 C 兼容类型。
- 导出函数可以使用 Feng 内部实现逻辑,但不得让异常越过 ABI 边界传播。
- 导出函数可以使用第 5 节中的调用方式注解显式声明 ABI; 未显式标注时默认按 `cdecl` 导出。
- 导出符号名默认等于函数声明名,当前语言版本不提供自定义符号重命名。
- 同一编译产物中若出现重名导出符号,编译期报错。
- `.fcp` 包中的头文件与导出清单由公开 `extern` 接口自动生成。

```feng
pu extern fn point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}
```

## 9 C互操作完整示例

```feng
pu mod libc.math;

// C 兼容结构体
extern type Point {
    var x: int;
    var y: int;
}

// C 兼容函数指针类型
extern type PointOperate(p: Point);

let point_lib = "./libpoint.so";

// 声明 C 外部函数
@cdecl(point_lib)
extern fn point_distance(p1: Point, p2: Point): float;

@cdecl(point_lib)
extern fn run_point_operate(p: Point, cb: PointOperate);

// 定义 Feng 回调函数
extern fn handle_point(p: Point) {
    print("Point:x=", p.x, " y=", p.y);
}

// 定义公开导出函数
pu extern fn point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}

// feng 内部普通函数
fn main(args: string[]) {
    // 初始化 C 兼容结构体
    let p1 = Point {x: 10, y: 20};
    let p2 = Point {x: 30, y: 40};

    // 调用 C 函数
    let dis = point_distance(p1, p2);
    print(dis);

    // 传递 feng 回调给 C
    run_point_operate(p1, handle_point);
}
```

## 10 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、C 互操作概要、模块、类型、函数、流程控制、异常、GC、包分发与完整示例。
- [feng-exception.md](./feng-exception.md): ABI 边界上的异常传播限制。
- 本文档: C 库来源与调用方式、C 兼容类型、外部函数声明、导出函数和回调规则的独立补充文档。
