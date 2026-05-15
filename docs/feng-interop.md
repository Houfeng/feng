# Feng 语言 ABI 互操作规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的 ABI 互操作概要说明,聚焦 Feng 语言与 C 库来源声明、ABI 兼容资格、`@abi` 声明、`Foo*` 函数指针、`extern fn` 导入声明、导出函数与回调规则。

> **设计原则基础**: 本文档建立在 [Feng 语言设计原则](./feng-principles.md) 之上。
> 尤其是: 注解只影响语义分析与代码生成,不改变语法; ABI 规则必须可在编译期判定; 互操作层不预设任何特定 C API 行为。

## 1 ABI互操作概览

- `extern fn` 仅用于声明 C 语言实现的外部函数,必须无函数体。
- 本文档是 C ABI 兼容资格的唯一权威来源; 其他规范文档只引用本文档,不重复枚举具体兼容集合。
- `@abi` 仅用于编译器做 ABI 兼容性检查,不引入新的运行时信息,不改变类型或函数值在 Feng 中的运行时表示。
- `@abi` 可写为无参形式或带一个目标参数; 当前无参等价于 `@abi("c")`,本文仅定义 `c` 目标语义; 未来可扩展为 `@abi("wasi")` 等其他目标。
- 当前版本中,`@abi` 仅适用于对象形式的 `type`、callable-form 的 `spec` 和顶层 `fn`; 对象形式的 `spec`、方法、lambda 与闭包都不是当前 `@abi` 目标。
- `@union` 仅适用于对象形式的 `@abi type`; 未标注 `@union` 时按 ABI 结构体 payload 处理,标注 `@union` 时按 ABI 联合体 payload 处理。
- 指针类型 `T*` 与函数指针类型 `Foo*` 在 Feng 中都是不透明句柄: 不可直接解引用、不可运算、不可比较、不可显式转换; `Foo*` 也不可直接调用。
- `string` 与 ABI 兼容数组在 ABI 边界上采用默认借用、优先 0 拷贝的规则; 具体 ABI 形状由 `extern fn` 签名显式表达,语言不预设未知 C API。
- 编译器或标准库需要暴露内建原生能力时,也必须通过普通 `extern fn` + 显式 ABI 签名表达; 这类符号归属 intrinsic 层,不归属 runtime 层。
- 顶层 `@abi fn` 可作为 ABI 回调来源,`pu @abi fn` 可作为公开导出函数; Feng 异常不得穿越 ABI 边界传播。

## 2 C库来源与调用方式注解

使用 `@cdecl("库名/路径")`、`@stdcall("库名/路径")` 或 `@fastcall("库名/路径")` 为无函数体的 `extern fn` 声明同时指定 C 库来源与调用方式。这三个注解在导入场景下的唯一参数支持以下两种写法:

1. 直接书写字符串字面量
2. 引用在当前可见作用域中、以字符串字面量直接初始化的 `let` 绑定; 来源文件或模块不限

无论采用哪种写法,编译器最终都会在编译期把参数解析为以下三种库来源之一:

1. 系统库名: 无特殊路径前缀,编译器自动补全系统库前缀和后缀
2. 相对路径: 以 `./` 或 `../` 开头,相对于当前 `.ff` 文件路径
3. 绝对路径: 以 `/` 开头,直接指定库文件完整路径

补充规则:

- `@cdecl("...")`、`@stdcall("...")` 和 `@fastcall("...")` 的带参数形式仅适用于无函数体的 `extern fn` 声明。
- 带参数的调用方式注解必须且只能带一个参数,该参数表示库名或路径。
- 若该参数使用 `let` 绑定引用,则该绑定必须以字符串字面量直接初始化,不可使用计算表达式或 `var` 绑定; 来源文件或模块不限,只要在使用点可见即可。
- 不同 `extern fn` 声明在同一文件或同一 `mod` 中可以指向不同原生库,不再要求“一个文件只归属于一个 C ABI 库”。
- 无函数体的 `extern fn` 声明必须且只能使用一个带参数的调用方式注解; 调用方式由注解名本身唯一确定。
- 无参数形式的 `@cdecl`、`@stdcall` 和 `@fastcall` 仅适用于顶层 `@abi fn`; 当前未显式标注时,顶层 `@abi fn` 默认按 `cdecl` 处理。
- 调用方式注解当前只对 `@abi("c")` 目标有定义。
- `@cdecl("feng_intrinsic")` 保留给编译器随附的 intrinsic 原生库; 标准库中的内建原生 API 必须放在该层并使用普通 ABI 签名,不得混入 runtime 公共 ABI。

```feng
let math_lib = "m";
let local_lib = "./libtest.so";

@cdecl(math_lib)
extern fn sin(x: float): float;

@stdcall(local_lib)
extern fn create_point(x: int, y: int): Point;

@cdecl("/usr/local/lib/libcurl.so")
extern fn curl_global_init(flags: u64): int;

@abi
@stdcall
pu fn create_point_export(x: int, y: int): Point {
    return Point { x: x, y: y };
}
```

## 3 ABI兼容资格

不在以下清单中的类型或函数,均视为 ABI 不兼容,编译期报错。

### 3.1 类型清单

| 类别 | 是否 ABI 兼容 | 条件 |
| --- | --- | --- |
| 基本标量类型 | 是 | 直接按 ABI 标量规则传递 |
| 指针类型 `U*` | 是 | 仅用于 ABI 边界传递; 在 Feng 表达式中不透明、不可直接操作 |
| 函数指针类型 `Foo*` | 是 | `Foo` 必须是 callable-form `@abi spec`; `Foo*` 仅作为不透明函数指针传递 |
| `@abi` 类型 | 是 | 类型本身通过 ABI 校验,且直接字段只允许当前白名单 |
| ABI 兼容数组 `T[]` | 有条件 | `T` 为基本标量、指针或已通过 ABI 校验的 `@abi` 类型,且元素按值连续存储 |
| `string` | 有条件 | 默认按借用方式参与 ABI 边界; 具体 ABI 形状由 `extern fn` 签名显式表达,语言不预设未知 C API |

### 3.2 函数清单

| 类别 | 是否 ABI 兼容 | 条件 |
| --- | --- | --- |
| 顶层 `fn` | 有条件 | 必须标注 `@abi`,且全部参数与返回值 ABI 兼容 |

补充规则:

- ABI 兼容资格必须由声明规则静态判定,不得依赖运行时猜测或按具体 C 库名称做特判。
- `extern fn` 参数位或返回位写成 `Foo*` 时,表示开发者声明该 ABI 位承载与 `Foo` 签名兼容的原生函数指针; 编译器只检查静态类型一致。
- `string` 与 ABI 兼容数组只在可调用 ABI 边界上定义; 它们不属于 `@abi type` 可直接内联的字段类型。

## 4 `@abi type` 与 `@union`

对象形式的 `type` 在标注 `@abi` 后,声明一个可供 ABI 校验与发码的 payload 视图; `@abi` 本身不改变该类型在 Feng 中的命名、对象语义、构造流程、默认零值、`==` / `!=` 语义、终结器规则或自动内存管理规则。

规则说明:

- `@abi` 不改变 `type` 的语法形式。某个 `type` 能否标记为 `@abi`,由语义分析按 ABI 规则检查。
- 带泛型形参的 `type` 不得标记 `@abi`; 任何泛型实例当前阶段也不参与 ABI 稳定校验。
- 对象形式的 `@abi type` 的直接字段类型只允许以下三类:
  1. 基本标量类型。
  2. 数据指针类型 `T*`,其中 `T` 只能是 `string`、ABI 兼容数组或已通过 ABI 校验的 `@abi` 类型。
  3. 函数指针类型 `Foo*`,其中 `Foo` 必须是 callable-form 的 `@abi spec`。
- 因此,`@abi type` 不允许直接把 `string`、数组、`@abi` 对象值或 callable-form `spec` 值本体内联为字段; 需要出现这些能力时必须通过对应的 `T*` 或 `Foo*` 字段表达。
- `@union` 仅适用于对象形式的 `@abi type`,不适用于 callable-form 的 `@abi spec`。
- 方法、构造函数、访问控制和注解本身都不参与 `@abi type` 的字段 ABI 校验; 方法定义不改变 payload 结果。
- `@abi type` 进入 ABI 边界时,传值或传指针完全由签名决定: 参数类型为 `T` 则按 ABI payload 值语义传递,参数类型为 `T*` 则传递该 payload 的地址; 编译器不做隐式兜底转换。
- `@abi type` 的字段 payload 是否需要显式释放,取决于外部资源拥有关系与外部协议,不能仅由 `@abi` 注解推导。

```feng
@abi
type Point {
    var x: int;
    var y: int;
}

@abi
type Slice {
    var data: byte*;
    var len: int;
}

@abi
type UserType2 {
    var p: Point;    // 编译期报错: `@abi type` 字段不能直接内联 `@abi` 对象
    var q: Point*;   // 合法
}

@abi
@union
type IntOrFloat {
    var i: int;
    var f: float;
}
```

## 5 `@abi spec`、`Foo*` 与 `@abi fn`

callable-form 的 `spec` 在标注 `@abi` 后,用于定义 ABI 函数签名类型。`@abi spec Foo(参数): 返回值;` 定义签名 `Foo`; `Foo*` 是对应的原生函数指针类型。

规则说明:

- `@abi spec` 仅适用于 callable-form `spec`; 对象形式的 `spec` 不得标记 `@abi`、`@union` 或调用方式注解。
- 编译器必须检查 `@abi spec` 的全部参数与返回值是否 ABI 兼容。
- `Foo` 本身仍是普通 callable-form `spec`,不引入新的运行时差异; `Foo*` 属于指针类型体系,与 `T*` 同级并遵循相同的不透明规则。
- `Foo*` 可直接用作 `extern fn` 的参数类型、返回类型以及 `@abi type` 的成员字段类型。
- 当前版本中,`@abi fn` 仅适用于顶层 `fn`; 方法、lambda、闭包、绑定方法值都不是合法的 `@abi fn`。
- 顶层 `@abi fn` 若要进入 ABI 边界,其全部参数与返回值必须 ABI 兼容。
- 顶层非公开 `@abi fn` 可作为 ABI 回调函数来源; 顶层 `pu @abi fn` 会生成公开的 C ABI 导出符号。
- `.fb` 包中的头文件与导出清单由公开 `@abi` 接口自动生成。
- `@abi fn` 内部若可能抛出异常,必须在函数体内捕获并转换为 C 侧可理解的返回约定; 未捕获异常不得穿越 ABI 边界传播。

```feng
@abi
spec CmpFunc(a: int, b: int): int;

@abi
fn int_cmp(a: int, b: int): int {
    return a - b;
}

@abi
pu fn point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}
```

## 6 一元 `&` 与指针来源

### 6.1 允许取址的对象

仅允许对以下五类值取 C 指针:

1. 基本标量
2. `@abi` 对象
3. `string`
4. ABI 兼容数组
5. 标注 `@abi` 且通过 ABI 检查的顶层 `fn`（目标类型必须显式给出为 `Foo*`）

其它类型一律报错,不做隐式兜底。

### 6.2 返回指针含义

1. `&scalar`: 返回该基本标量存储单元的首地址指针,类型 `T*`。
2. `&abi_value`: 返回该 `@abi` 值对应 ABI payload 的首地址指针,类型 `T*`。
3. `&str`: 返回 `string` 的 ABI 兼容数据地址指针,类型 `string*`; 当前 `c` 目标下该地址为 UTF-8 数据区首地址。
4. `&arr`: 返回 ABI 兼容数组第 `0` 个元素地址; 空数组返回 `0` 指针,类型 `T*`。
5. `&abi_fn`: 在目标类型显式给出为 `Foo*` 时,返回该顶层 `@abi fn` 的函数指针。

### 6.3 可写性与生命周期

1. `&string` 结果不在语言层强制只读; 是否允许写入由调用方声明的 C 契约决定。若写入破坏 Feng `string` 约束,行为未定义。
2. `&array` 是否可写由数组可写层语义决定。
3. `&abi_value` 是否可写由绑定可变性与成员可写规则共同决定。
4. `&scalar` 是否可写由标量绑定的可变性决定。
5. 数据指针默认借用,仅保证调用期间有效; C 侧若可能缓存、异步使用或以其他方式逃逸使用,开发者必须显式保活 owner。
6. 函数指针 `Foo*` 本身无生命周期问题,但与之配套传递的 `user_data` 等附带对象仍由调用方负责保活。
7. 允许对临时值取数据指针; 编译器不做自动延寿。若调用返回后仍需继续使用,开发者必须自行保活 owner。

### 6.4 函数指针来源与禁止项

`Foo*` 可来自以下位置:

1. 对满足条件的顶层 `@abi fn` 执行 `&` 取址。
2. `extern fn` 返回值。
3. 已有 `Foo*` 绑定、参数、字段或返回值的继续传递。

补充规则:

- 对顶层 `@abi fn` 执行 `&` 时,目标类型必须显式给出为 `Foo*`; 编译器据此检查该 `fn` 与 `Foo` 的参数和返回值完全一致。
- 缺少目标类型、存在重载歧义或 `fn` 与 `Foo` 签名不一致时,编译器必须报错。
- 以下所有情况都禁止作为 `Foo*` 的来源: 未标注 `@abi` 的顶层函数、成员函数、lambda、闭包、绑定方法值以及任何带环境捕获的 callable 值。
- 除 `&abi_fn` 场景外,编译器通常无法追溯 `Foo*` 的真实来源是否匹配 `Foo` 签名; 这类 ABI 正确性由开发者通过 `extern fn` 声明或外部 API 契约保证。

### 6.5 指针类型安全

- 一元 `&` 的结果类型为 `T*` 或 `Foo*`,与取址目标相匹配。
- 指针在 Feng 中只可用于 ABI 边界的存储、传递和返回,不作为通用编程能力开放。
- 凡超出上述边界的解引用、调用、运算、比较或显式转换,编译期都必须报错。
- 二元 `&` 继续保留为按位与,一元 `&` 仅表示取地址,两者按语法位置区分。

```feng
@cdecl("c_use_i32_ptr")
extern fn c_use_i32_ptr(p: int*): void;

let x: int = 42;
let p: int* = &x;
c_use_i32_ptr(p);

@abi
spec PointOperate(p: Point): void;

@abi
fn handle_point(p: Point) {
    print(p.x, p.y);
}

let cb: PointOperate* = &handle_point;

@cdecl("./libpoint.so")
extern fn run_point_operate(p: Point, cb: PointOperate*): void;
```

## 7 `string` 的 ABI 0拷贝规则

`string` 在 ABI 边界只定义借用与 0 拷贝能力; 当 ABI 位写成 `string*` 时,表示该位承载 `string` 的 ABI 兼容数据地址; 当前 `c` 目标下该地址为 UTF-8 数据区首地址。

规则说明:

- `extern fn` 若需要字符串数据指针,必须在签名中显式声明对应指针类型（如 `string*`）,并在调用点显式取址（如 `&str`）; 不允许把 `string` 作为入参自动转换为指针。
- 默认只保证调用期间有效。
- 若调用方声明 C 侧会在调用返回后继续使用该指针,开发者必须显式持有原始 `string` owner。
- `string` 不属于 `@abi type` 可直接内联的字段类型; 若需要在 `@abi type` 中表达相关数据,必须改用显式指针与长度字段。
- 若某个 C API 明确要求可写 `char*` 缓冲,Feng 侧可将该 ABI 位显式声明为 `string*`; 调用点仍必须通过一元 `&` 传入 ABI 兼容数据地址而非依赖自动转换。若 C 写入破坏字符串约束（如 UTF-8 / 长度一致性等）,行为未定义。

## 8 ABI兼容数组

数组 `T[]` 可作为 ABI 兼容数组的前提是: 元素类型 `T` 满足以下之一:

1. 基本标量类型
2. 指针类型 `U*`
3. 已通过 ABI 校验的 `@abi` 类型

额外硬约束:

- 元素在数组中必须按值连续存储,不能是引用槽位语义。
- ABI 兼容数组的长度不跟随 `&` 一起隐式传递,长度必须作为显式字段或显式参数表达。
- 数组自身不属于 `@abi type` 可直接内联的字段类型; 需要在 `@abi type` 中表达数组数据时,必须改用显式数据指针与长度字段。
- ABI 数组使用点无需再次递归遍历成员,只查询元素类型 `T` 是否已通过当前 ABI 资格检查。

## 9 开发者责任与诊断

开发者必须根据调用方声明的 C 契约判断是否需要延长引用寿命:

1. 同步只读调用: 可用默认借用与 0 拷贝。
2. 可能逃逸: 必须显式保活 owner（例如 wrapper 成员持有原始 `string`、数组或 `@abi` 对象）。
3. 不允许把“缓存裸指针”误当作“已保活对象”。
4. `extern fn` 的参数位或返回位若写成 `Foo*`,其 ABI 真实性由开发者保证,编译器不证明外部实现与声明完全一致。

推荐诊断风格示例:

- `type BufferList` 含有不能直接内联到 `@abi type` 的字段 `items`,因此不能标记为 `@abi`。
- `fn point_sum` 的参数 `user` 类型不是 ABI 兼容类型,因此该函数不能标记为 `@abi`。
- `spec PointHandler` 是对象形式的 `spec`,不能标记为 `@abi`; `@abi` 的 `spec` 仅适用于 callable-form。
- `let cb = &int_cmp` 缺少显式目标 `Foo*` 类型,因此不能确定函数指针类型。

## 10 C互操作完整示例

```feng
pu mod libc.math;

@abi
type Point {
    var x: int;
    var y: int;
}

@abi
spec PointOperate(p: Point): void;

let point_lib = "./libpoint.so";

@cdecl(point_lib)
extern fn point_distance(p1: Point, p2: Point): float;

@cdecl(point_lib)
extern fn run_point_operate(p: Point, cb: PointOperate*): void;

@abi
fn handle_point(p: Point) {
    print("Point:x=", p.x, " y=", p.y);
}

@abi
pu fn point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}

fn main(args: string[]) {
    let p1 = Point { x: 10, y: 20 };
    let p2 = Point { x: 30, y: 40 };

    let dis = point_distance(p1, p2);
    print(dis);

    let cb: PointOperate* = &handle_point;
    run_point_operate(p1, cb);
}
```

## 11 与主规范的关系

- [feng-principles.md](./feng-principles.md): 语言设计原则、分层原则与诊断原则。
- [feng-language.md](./feng-language.md): 语言总体规范、ABI 互操作概要、模块、类型、函数、流程控制、异常、自动内存管理、包分发与完整示例。
- [feng-type.md](./feng-type.md): `@abi type` 仍是 Feng `type`,对象模型与构造/终结器规则不因 `@abi` 改变。
- [feng-lifetime.md](./feng-lifetime.md): 托管对象、原始指针与 ABI 借用边界的生命周期规则。
- [feng-exception.md](./feng-exception.md): ABI 边界上的异常传播限制。
- 本文档: C 库来源与调用方式、ABI 兼容资格、`@abi`、`Foo*`、`extern fn` 导入声明、导出函数和回调规则的独立补充文档。
