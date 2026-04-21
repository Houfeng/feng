# Feng 语言 C 互操作规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的 C 互操作概要说明,聚焦 Feng 语言与 C 库来源声明、C 兼容类型、外部函数声明、导出函数和回调函数定义规则。

> **设计原则基础**: 本文档建立在 [Feng 语言设计原则](./feng-principles.md) 之上。
> 尤其是: 注解只影响语义分析与代码生成,不改变语法; 只有关键字能够影响语法类别,语法问题应能在 parse 阶段检查出来。

## 1 C互操作概览

- `extern fn` 仅用于声明 C 语言实现的外部函数,必须无函数体。
- `@fixed` 用于标记 `type`、`fn` 或方法希望进入 ABI 固定边界; 它表达的是语义资格,不是新的语法类别。
- 对象形式的 `@fixed type` 用于定义 C 兼容结构体; `@fixed @union type` 用于定义 C 兼容联合体。
- 函数形式的 `@fixed type` 用于定义 C 兼容函数指针类型。
- 带函数体的 `@fixed fn` 可用于定义 Feng 实现的 ABI 兼容回调函数; 顶层 `pu @fixed fn` 可用于定义公开导出的 C ABI 函数。
- 带参数的 `@cdecl`、`@stdcall` 和 `@fastcall` 只用于无函数体的 `extern fn` 导入声明; 无参数形式只用于 `@fixed fn` 或 `@fixed` 方法的 ABI 调用方式。
- `@fixed` 的合法性由语义分析检查。诊断应表述为“某声明不能标记为 `@fixed`”,而不是“`@fixed` 改写了语法”。

## 2 C库来源与调用方式注解

使用 `@cdecl("库名/路径")`、`@stdcall("库名/路径")` 或 `@fastcall("库名/路径")` 为无函数体的 `extern fn` 声明同时指定 C 库来源与调用方式。这三个注解在导入场景下的唯一参数支持以下两种写法:

1. 直接书写字符串字面量
2. 引用当前 `mod` 作用域中以字符串字面量初始化的 `let` 绑定

无论采用哪种写法,编译器最终都会在编译期把第一个参数解析为以下三种库来源之一:

1. 系统库名: 无特殊路径前缀,编译器自动补全系统库前缀和后缀
2. 相对路径: 以 `./` 或 `../` 开头,相对于当前 `.f` 文件路径
3. 绝对路径: 以 `/` 开头,直接指定库文件完整路径

补充规则:

- `@cdecl("...")`、`@stdcall("...")` 和 `@fastcall("...")` 的带参数形式仅适用于无函数体的 `extern fn` 声明。
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

## 3 C兼容结构体与联合体定义(@fixed type)

对象形式的 `type` 在标注 `@fixed` 后,可定义可与 C 直接映射的结构体或联合体,无需 C 侧额外声明。未显式标注 `@union` 时,对象形式的 `@fixed type Name { ... }` 默认按 C 结构体处理; 若需声明为 C 联合体,可在 `@fixed type` 上再添加 `@union` 注解。

规则说明:

- `@fixed` 不改变 `type` 的语法形式。某个 `type` 能否标记为 `@fixed`,由语义分析按 ABI 稳定类型集合检查。
- 对象形式的 `@fixed type` 的直接字段类型必须属于 ABI 稳定类型集合。
- 在当前语言版本中,ABI 稳定类型集合至少包含: 基础类型、其他合法的 `@fixed type`、合法的 `@fixed` 函数类型和固定长度数组。
- `@union` 仅适用于对象形式的 `@fixed type`,不适用于函数指针形式的 `@fixed type`。
- 联合体的所有成员共享同一块内存,其有效成员语义与 C 联合体保持一致。
- `@fixed type` 和 `@fixed @union type` 的实例布局仅由字段决定; 方法、构造函数、访问控制和注解本身都不参与实例内存布局计算。
- 编译器不得为 `@fixed type` 或 `@fixed @union type` 注入对象头、虚表指针、判别标签或其他隐藏实例字段。
- `@fixed type` 与 `@fixed @union type` 是非托管 ABI 值类型。运行时不以 RC 或 GC 管理其生命周期; 是否需要显式释放取决于它们的存储位置和资源拥有关系。
- 对象形式的 `@fixed type` 可以声明普通方法或构造函数,但它们不自动进入 ABI 边界; 方法只有在其自身也标记为 `@fixed` 时,才按 ABI 兼容函数检查。

```feng
@fixed
type Point {
    var x: int;
    var y: int;
}

@fixed
type Rect {
    var p1: Point;
    var p2: Point;
    var area: float;
}

@fixed
@union
type IntOrFloat {
    var i: int;
    var f: float;
}
```

## 4 C函数指针类型定义(@fixed type)

函数形式的 `type` 在标注 `@fixed` 后,用于定义与 C 函数指针兼容的函数类型,以支持回调函数传递。函数指针类型本身不使用 `@union` 或调用方式注解; 调用方式由与之匹配的 `extern fn` 导入声明,或 `@fixed fn` / `@fixed` 方法定义负责标注。

规则说明:

- `@fixed type Name(参数): 返回值;` 定义 C 兼容函数指针类型。
- 函数指针类型的直接参数类型和返回类型必须属于 ABI 稳定类型集合。
- `@union` 不适用于函数形式的 `@fixed type`。

```feng
@fixed
type CmpFunc(a: int, b: int): int;

@fixed
type PointCallback(p: Point);
```

## 5 Feng实现的 `@fixed fn` 调用方式注解

Feng 提供 `@cdecl`、`@stdcall` 和 `@fastcall` 三个内建注解,用于显式声明带函数体的 `@fixed fn` 或 `@fixed` 方法的 C ABI 调用方式。在这类场景中,调用方式注解不得带参数。

规则说明:

- `@cdecl`: 无参数时声明使用 C 默认调用方式。
- `@stdcall`: 无参数时声明使用 `stdcall` 调用方式。
- `@fastcall`: 无参数时声明使用 `fastcall` 调用方式。
- 每个 `@fixed fn` 或 `@fixed` 方法最多只能使用一个调用方式注解。
- 未显式标注时,`@fixed fn` 与 `@fixed` 方法默认按 `cdecl` 处理。
- 调用方式注解通常单独写在 `@fixed fn` 或方法定义的上一行,以保持书写清晰。

```feng
@fixed
fn my_int_cmp(a: int, b: int): int {
    return a - b;
}

@fixed
@stdcall
pu fn create_point_export(x: int, y: int): Point {
    return Point { x: x, y: y };
}
```

## 6 C外部函数声明(extern fn)

使用无函数体的 `extern fn` 声明 C 语言实现的外部函数,用于调用 C 库函数。`extern fn` 属于语法类别,因此“是否带函数体”必须在 parse 阶段直接检查出来。

规则说明:

- `extern fn` 声明必须无函数体。
- `extern fn` 声明必须使用且只能使用一个带参数的 `@cdecl`、`@stdcall` 或 `@fastcall`。
- 带参数注解的参数可以是字符串字面量,也可以是当前 `mod` 作用域中以字符串字面量初始化的 `let` 绑定。
- 带参数注解的注解名本身决定调用方式,不再额外传入字符串形式的调用约定。
- `extern fn` 的参数和返回值应使用语言已定义的 C 兼容表示。

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

## 7 Feng 回调函数与 C ABI导出函数(@fixed fn)

使用带函数体的 `@fixed fn` 可定义 Feng 实现的 ABI 兼容函数。此类函数既是合法的 Feng 函数,又接受 ABI 边界规则约束。

规则说明:

- 顶层非公开 `@fixed fn` 可作为回调函数传给 C。
- 顶层 `pu @fixed fn` 会生成公开的 C ABI 导出符号。
- 方法只有在其自身标记为 `@fixed` 时,才会进入 ABI 兼容检查; 普通方法即使定义在 `@fixed type` 上,也不自动成为 C ABI 接口。
- `@fixed fn`、`@fixed` 方法和 `pu @fixed fn` 的直接参数类型与返回类型必须属于 ABI 稳定类型集合。
- `@fixed fn` 和 `@fixed` 方法不得捕获外部变量,也不得使用闭包作为 ABI 可调用值。
- 未捕获异常不得穿越 `@fixed` ABI 边界传播。
- `.fcp` 包中的头文件与导出清单由公开 `@fixed` 接口自动生成。

```feng
@fixed
fn my_point_handle(p: Point) {
    print(p.x, p.y);
}

@fixed
pu fn point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}
```

## 8 ABI兼容资格与生命周期

`@fixed` 的合法性属于语义资格检查,而不是语法检查。

规则说明:

- `@fixed type` 的直接字段类型、固定数组元素类型和 `@fixed` 函数类型的直接参数/返回值,必须属于 ABI 稳定类型集合。
- 某个具名类型只有在其自身也通过 `@fixed` 合法性校验后,才属于 ABI 稳定类型集合。
- 普通 `type` 可以包含 `@fixed` 值字段; 但 `@fixed` 声明不能直接依赖普通 `type`、`string`、动态数组、闭包或其他托管对象类型。
- 生命周期不由 `@fixed` 自动变成“总是手动释放”; 更准确的规则是: `@fixed` ABI 值不受运行时托管,其生命周期由存储位置和资源拥有关系决定。
- 若 `@fixed` 值通过外部分配获得或内部持有外部资源,则应通过显式的 `free`、`close` 或同类协议释放。

推荐诊断风格示例:

- `type BufferList` 含有非 `@fixed` 的成员 `items`,因此不能标记为 `@fixed`。
- `fn point_sum` 的参数 `user` 类型不是 ABI 稳定类型,因此该函数不能标记为 `@fixed`。

## 9 C互操作完整示例

```feng
pu mod libc.math;

@fixed
type Point {
    var x: int;
    var y: int;
}

@fixed
type PointOperate(p: Point);

let point_lib = "./libpoint.so";

@cdecl(point_lib)
extern fn point_distance(p1: Point, p2: Point): float;

@cdecl(point_lib)
extern fn run_point_operate(p: Point, cb: PointOperate);

@fixed
fn handle_point(p: Point) {
    print("Point:x=", p.x, " y=", p.y);
}

@fixed
pu fn point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}

fn main(args: string[]) {
    let p1 = Point {x: 10, y: 20};
    let p2 = Point {x: 30, y: 40};

    let dis = point_distance(p1, p2);
    print(dis);

    run_point_operate(p1, handle_point);
}
```

## 10 与主规范的关系

- [feng-principles.md](./feng-principles.md): 语言设计原则、分层原则与诊断原则。
- [feng-language.md](./feng-language.md): 语言总体规范、C 互操作概要、模块、类型、函数、流程控制、异常、GC、包分发与完整示例。
- [feng-exception.md](./feng-exception.md): ABI 边界上的异常传播限制。
- 本文档: C 库来源与调用方式、`@fixed` ABI 类型、`extern fn` 导入声明、导出函数和回调规则的独立补充文档。
