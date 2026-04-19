# feng语言C互操作规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的 C 互操作概要说明,聚焦 feng 语言与 C 库链接、C 兼容类型、外部函数声明和回调函数定义规则。

## 1 C互操作概览

- feng 通过 `link`、`extern type` 和 `extern fn` 实现与 C 语言的安全互操作。
- `link` 负责链接 C 标准库、第三方 C 库或 feng 编译二进制产物。
- `extern type` 用于定义 C 兼容结构体和 C 函数指针类型。
- `extern fn` 用于声明 C 外部函数以及定义可传给 C 的 feng 回调函数。

## 2 C库链接指令(link)

使用 `link "库名/路径"` 指令链接 C 库或 feng 编译二进制库。`link` 声明必须位于 `mod` / `use` 段之后、其他语句之前; 若文件中存在 `use` 导入段,则必须位于连续 `use` 段之后; 若不存在 `use`,则位于 `mod` 声明之后。编译器自动识别链接类型,支持三种形式:

1. 系统库名: 无特殊路径前缀,编译器自动补全系统库前缀和后缀
2. 相对路径: 以 `./` 或 `../` 开头,相对于当前 `.f` 文件路径
3. 绝对路径: 以 `/` 开头,直接指定库文件完整路径

补充规则:

- 一个文件只能归属于一个 C ABI 库声明。
- 同一个 C ABI 库声明可以分布在多个文件中,编译时统一合并为同一 C ABI 库。
- 建议将 `link` 声明作为独立链接声明段处理,其上方和下方各保留一行空行。

```feng
// 链接系统数学库
link "m";

// 链接当前目录自定义 C 库
link "./libtest.so";

// 链接绝对路径第三方 C 库
link "/usr/local/lib/libcurl.so";
```

书写顺序示例:

```feng
pu mod libc.math;

use libc.base;
use libc.extra;

link "m";

extern fn sin(x: float): float;
```

## 3 C兼容结构体定义(extern type)

使用 `extern type` 定义可与 C 直接映射的结构体,无需 C 侧额外声明。其内存布局、字节序、内存对齐与 C 语言完全一致,可直接在 feng 与 C 之间按值或按指针传递,并直接读写成员。

语法规则:

- 仅允许声明成员变量,禁止添加构造函数、成员方法
- 成员仅支持: 基础类型、其他 `extern type` 类型、固定长度数组
- 成员需用 `var` 或 `let` 修饰,遵循 feng 变量声明规范
- feng 的 GC 不管理该类型内存,需通过 C 函数或手动释放
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
```

## 4 C函数指针类型定义(extern type)

使用 `extern type` 定义与 C 函数指针完全兼容的函数类型,用于 C 回调函数传递,签名需与 C 侧完全一致。

```feng
// 定义 C 兼容的比较函数指针类型
extern type CmpFunc(a: int, b: int): int;

// 定义 C 兼容的结构体回调函数类型
extern type PointCallback(p: Point);
```

## 5 C外部函数声明(extern fn)

使用 `extern fn` 声明 C 语言实现的外部函数,仅声明函数签名、无函数体,用于调用 C 库函数。参数和返回值需为 C 兼容类型,如基础类型、`extern type` 类型和指针。

```feng
// 声明 C 标准库数学函数
extern fn sin(x: float): float;
extern fn sqrt(x: float): float;

// 声明操作 C 兼容结构体的 C 函数
extern fn create_point(x: int, y: int): Point;
extern fn set_point_callback(cb: PointCallback);
```

## 6 feng回调函数定义(extern fn)

使用 `extern fn` 定义可作为函数指针传给 C 的 feng 回调函数。此类函数有函数体,编译器自动生成 C 兼容 ABI,并进行强制编译期检查。

约束规则:

- 禁止捕获外部变量
- 禁止使用闭包
- 仅支持 C 兼容类型作为参数和返回值
- 不可使用 feng GC 托管类型,如 `string`、动态数组、原生 `type` 对象

```feng
// 可传给 C 的比较回调函数
extern fn my_int_cmp(a: int, b: int): int {
    return a - b;
}

// 可传给 C 的结构体回调函数
extern fn my_point_handle(p: Point) {
    print(p.x, p.y);
}
```

## 7 C互操作完整示例

```feng
pu mod libc.math;

link "m";

// C 兼容结构体
extern type Point {
    var x: int;
    var y: int;
}

// C 兼容函数指针类型
extern type PointOperate(p: Point);

// 声明 C 外部函数
extern fn point_distance(p1: Point, p2: Point): float;
extern fn run_point_operate(p: Point, cb: PointOperate);

// 定义 feng 回调函数
extern fn handle_point(p: Point) {
    print("Point:x=", p.x, " y=", p.y);
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

## 8 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、C 互操作概要、模块、类型、函数、流程控制、异常、GC、包分发与完整示例。
- 本文档: C 库链接、C 兼容类型、外部函数声明和回调规则的独立补充文档。
