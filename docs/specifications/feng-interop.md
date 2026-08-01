# Feng 语言 ABI 互操作规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的 ABI 互操作概要说明,聚焦 Feng 语言在 C ABI 路径下的 C 库来源声明、ABI 兼容资格、`@abi` 声明、`Foo*` 函数指针、`extern func` 导入声明、回调规则与未来公开导出 surface 规则。

> **设计原则基础**: 本文档建立在 [Feng 语言设计原则](./feng-principles.md) 之上。
> 尤其是: 注解只影响语义分析与代码生成,不改变语法; ABI 规则必须可在编译期判定; 互操作层不预设任何特定 C API 行为。

## 1 ABI互操作概览

- `extern func` 是语言中的统一外部函数声明语法,必须无函数体; `extern` 只能用于顶层 `func`,不得用于 `type`、`enum`、`spec`、`fit`、模块级 `let` / `var` 或其他声明。本文档只定义 `extern func` 在 C ABI 路径下的导入规则。
- 本文档是 C ABI 导入规则与 ABI 兼容资格的唯一权威来源; 其他规范文档只引用本文档,不重复枚举具体兼容集合。
- `@abi` 仅用于编译器做 ABI 兼容性检查,不引入新的运行时信息,不改变类型或函数值在 Feng 中的运行时表示。
- `@abi` 可写为无参形式或带一个目标参数; 当前无参等价于 `@abi("c")`,本文仅定义 `c` 目标语义; 未来可扩展为 `@abi("wasi")` 等其他目标。
- 当前版本中,`@abi` 仅适用于对象形式的 `type`、callable-form 的 `spec` 和顶层 `func`; 对象形式的 `spec`、方法、lambda 与闭包都不是当前 `@abi` 目标。
- 指针类型 `T*` 与函数指针类型 `Foo*` 在 Feng 中都是不透明句柄: 不可直接解引用、不可运算、不可显式转换; `Foo*` 也不可直接调用。仅允许同类型指针参与 `==` / `!=`,结果按原生指针地址身份判定。
- `string` 与 ABI 兼容数组在 ABI 边界上采用默认借用、优先 0 拷贝的规则; 具体 ABI 形状由 `extern func` 签名显式表达,语言不预设未知 C API。
- 编译器私有的 runtime contract helper 不属于本文定义的公共 C ABI 规则; 本文不展开这类内部入口。
- 顶层 `@abi func` 仍然是 Feng 自己实现的函数; 公开或非公开形态都可作为 ABI 回调来源。仅 `open @abi func` 在语义上保留为未来面向其他语言的 C ABI 导出 surface; Feng 异常不得穿越 ABI 边界传播。

除非特别说明,本文中的 `extern func` 均指带 `@cdecl(...)` / `@stdcall(...)` / `@fastcall(...)` 的 C ABI 导入声明; 其他非 C ABI 外部目标不在本文范围内。

## 2 C库来源与调用方式注解

使用 `@cdecl(库名)` / `@cdecl(库名, C函数名)` / `@cdecl(库名, C函数名, 固定参数个数)`、`@stdcall(...)` 或 `@fastcall(...)` 为无函数体的 `extern func` 声明指定 C ABI 导入路径下的 C 库来源、调用方式、可选 C 函数名与可选 C variadic 固定参数个数。第一个参数表示库名; 第二个参数表示实际导入的 C 函数名; 第三个参数表示 C variadic 原型中的固定参数个数。省略第二个参数时,C 函数名等于 `extern func` 的 Feng 声明名。

库名和 C 函数名均支持以下两种写法:

1. 直接书写字符串字面量
2. 引用在当前可见作用域中、以字符串字面量直接初始化的 `let` 绑定; 来源文件或模块不限

固定参数个数必须是整数字面量。省略或传入 `0` 均表示非 C variadic; 大于 `0` 表示 C variadic,有效范围为 `1..param_count`,其中 `param_count` 是该 Feng 声明的参数总数。生成的 C 原型将前 N 个参数声明为固定参数,其余 Feng 声明参数通过 C `...` 传递。N 等于 `param_count` 时仍生成 C variadic 原型,但该 Feng 声明不传入可变参数。

编译器最终在编译期解析库名、C 函数名和固定参数个数。库名无特殊路径前缀,并按系统规则补全库前后缀。C 函数名作为链接器原生符号名。固定参数个数必须在本包编译和跨包导入中保持一致。

补充规则:

- `@cdecl("...")`、`@stdcall("...")` 和 `@fastcall("...")` 的带参数形式仅适用于无函数体的 `extern func` 声明。
- 带参数的调用方式注解必须带一至三个参数。第三个参数只能在第二个参数之后出现。
- 若库名或 C 函数名使用 `let` 绑定引用,则该绑定必须以字符串字面量直接初始化,不可使用计算表达式或 `var` 绑定; 来源文件或模块不限,只要在使用点可见即可。
- 省略第二个参数时,代码生成继续使用 `extern func` 的 Feng 声明名作为 C 函数名,保持既有行为。
- 不同 `extern func` 声明在同一文件或同一 `module` 中可以指向不同原生库,不再要求“一个文件只归属于一个 C ABI 库”。
- C ABI 路径下,无函数体的 `extern func` 声明必须且只能使用一个带参数的调用方式注解; 调用方式由注解名本身唯一确定。
- 对带参数的 `extern func` 导入声明,代码生成必须把调用方式差异带入主机 C 声明,并使用解析后的 C 函数名作为主机 C 声明与调用符号: `@cdecl` 使用默认 C 调用约定,`@stdcall` / `@fastcall` 在主机工具链提供独立调用约定关键字或属性时必须显式发射; 若当前目标 ABI 不区分这些约定,可退化为默认声明,但不得丢失编译期元信息。
- C ABI 路径下,带类型参数的 `extern func` 仅在每个参数位与返回位抹除类型参数后仍对应唯一且 ABI-stable 的 C surface 时才合法; 若某一位置会随具体类型实参改变 C surface,则编译器必须拒绝该声明。调用这类泛型 `extern func` 时,类型实参既可显式给出,也可在能由实参 ABI surface 唯一确定时省略; 无法唯一确定时必须报错。当前这条按包裹 ABI surface 递归推导的省略规则仅服务于 `extern func` 导入调用,不改变普通 Feng 函数调用的泛型推导范围。
- 无参数形式的 `@cdecl`、`@stdcall` 和 `@fastcall` 仅适用于顶层 `@abi func`; 它们描述的是该 Feng 函数进入 ABI 边界时的调用方式,不改变其“由 Feng 提供实现”的语义身份。当前未显式标注时,顶层 `@abi func` 默认按 `cdecl` 处理。
- 调用方式注解当前只对 `@abi("c")` 目标有定义。

```feng
let math_lib = "m";
let local_lib = "test";
let c_fabs = "fabs";

@cdecl(math_lib)
extern func sin(x: float): float;

@cdecl(math_lib, c_fabs)
extern func abs_value(x: double): double;

@stdcall(local_lib, "create_point")
extern func create_point(x: int, y: int): Point;

@cdecl("libc", "snprintf", 3)
extern func snprintf_f64(buf: byte*, size: uint, fmt: byte*, value: f64): i32;

@abi
@stdcall
open func create_point_export(x: int, y: int): Point {
    return Point { x: x, y: y };
}
```

## 3 ABI兼容资格

不在以下清单中的类型或函数,均视为 ABI 不兼容,编译期报错。

### 3.1 类型清单

| 类别 | 是否 ABI 兼容 | 条件 |
| --- | --- | --- |
| 基本标量类型 | 是 | 直接按 ABI 标量规则传递 |
| `enum` | 是 | 视为与 `int` 相同的 ABI 标量；可直接按值进入 ABI 边界 |
| 指针类型 `U*` | 是 | 仅用于 ABI 边界传递; 在 Feng 表达式中不透明、不可直接操作 |
| 函数指针类型 `Foo*` | 是 | `Foo` 必须是 callable-form `@abi spec`; `Foo*` 仅作为不透明函数指针传递 |
| `@abi` 类型 | 有条件 | 对象形式 `@abi type` 有字段时可按值进入 ABI; 无字段时仅可作为 `T*` 的名义 pointee |
| ABI 兼容数组 `T[]` | 有条件 | `T` 为基本标量、指针或已通过 ABI 校验的 `@abi` 类型,且元素按值连续存储 |
| `string` | 有条件 | 默认按借用方式参与 ABI 边界; 具体 ABI 形状由 `extern func` 签名显式表达,语言不预设未知 C API |

### 3.2 函数清单

| 类别 | 是否 ABI 兼容 | 条件 |
| --- | --- | --- |
| 顶层 `func` | 有条件 | 必须标注 `@abi`,且全部参数与返回值 ABI 兼容 |

补充规则:

- ABI 兼容资格必须由声明规则静态判定,不得依赖运行时猜测或按具体 C 库名称做特判。
- `enum` 在 ABI 边界上的按值表示与 `int` 完全一致; ABI 兼容性检查按 `int` 标量规则处理,但语言层仍保持 `enum` 是独立具名类型。
- `extern func` 参数位或返回位写成 `Foo*` 时,表示开发者声明该 ABI 位承载与 `Foo` 签名兼容的原生函数指针; 编译器只检查静态类型一致。
- `string` 与 ABI 兼容数组只在可调用 ABI 边界上定义; 它们不属于 `@abi type` 可直接内联的字段类型。

## 4 `@abi type`

对象形式的 `type` 在标注 `@abi` 后,声明一个可供 ABI 校验与发码的 payload 视图; `@abi` 本身不改变该类型在 Feng 中的命名、对象语义、构造流程、默认零值、`==` / `!=` 语义或自动内存管理规则; `@abi` 与终结器互斥,标注 `@abi` 的类型不得声明终结器。

规则说明:

- `@abi` 不改变 `type` 的语法形式。某个 `type` 能否标记为 `@abi`,由语义分析按 ABI 规则检查。
- 带泛型形参的 `type` 不得标记 `@abi`; 任何泛型实例当前阶段也不参与 ABI 稳定校验。
- 对象形式的 `@abi type` 按是否声明字段分成两类: 有字段者可以按值进入 C ABI surface,也可以按取址规则形成 `T*`; 无字段者只作为 `T*` 的名义 pointee。
- 没有 `@abi` 的 `type` 不能进入 C 边界。
- 对象形式的 `@abi type` 的直接字段类型只允许以下三类:
    1. 基本标量类型与 `enum` 类型。
    2. 数据指针类型 `T*`,其中 `T` 只能是 `string`、ABI 兼容数组或已通过 ABI 校验的 `@abi` 类型。
    3. 函数指针类型 `Foo*`,其中 `Foo` 必须是 callable-form 的 `@abi spec`。
- 因此,`@abi type` 不允许直接把 `string`、数组、`@abi` 对象值或 callable-form `spec` 值本体内联为字段; 需要出现这些能力时必须通过对应的 `T*` 或 `Foo*` 字段表达。
- 方法、构造函数、访问控制和注解本身都不参与 `@abi type` 的字段 ABI 校验; 方法定义不改变 payload 结果。
- `@abi type` 进入 ABI 边界时,传值或传指针完全由签名决定: 参数类型为 `T` 则按 ABI payload 值语义传递,参数类型为 `T*` 则传递该 payload 的地址; 编译器不做隐式兜底转换。
- 在 `extern func`、顶层 `@abi func` 与 callable-form `@abi spec` 中,无字段对象形式 `@abi type` 只能以 `T*` 形态出现,不能按值写成 `T`。
- 当 `extern func` 或顶层 `@abi func` 的参数位写成 `T`（其中 `T` 是声明了字段的对象形式 `@abi type`）时,该 ABI 位在 C surface 上使用隐藏的 `T__AbiLayout` 结构按值传递; 进入 Feng 函数体时,编译器会把该 payload 装箱为新的托管 `T` 实例供后续语义继续使用。
- 当 `extern func` 或顶层 `@abi func` 的返回位写成 `T`（其中 `T` 是声明了字段的对象形式 `@abi type`）时,该 ABI 位同样按隐藏的 `T__AbiLayout` 值语义返回; 从 C 进入 Feng 时,编译器必须把返回的 payload 重建为新的托管 `T` 实例,而从 Feng 导出到 ABI surface 时,编译器必须从 `T` 对象中抽取出对应 payload 返回。
- 无字段对象形式 `@abi type` 不生成按值 `T__AbiLayout`; 其 `T*` 在 C surface 上按 opaque pointer 处理。
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

```

## 5 `@abi spec`、`Foo*` 与 `@abi func`

callable-form 的 `spec` 在标注 `@abi` 后,用于定义 ABI 函数签名类型。`@abi spec Foo(参数): 返回值;` 定义签名 `Foo`; `Foo*` 是对应的原生函数指针类型。

规则说明:

- `@abi spec` 仅适用于 callable-form `spec`; 对象形式的 `spec` 不得标记 `@abi` 或调用方式注解。
- 编译器必须检查 `@abi spec` 的全部参数与返回值是否 ABI 兼容; 其中无字段对象形式 `@abi type` 只能以 `T*` 形态出现。
- `Foo` 本身仍是普通 callable-form `spec`,不引入新的运行时差异; `Foo*` 属于指针类型体系,与 `T*` 同级并遵循相同的不透明规则。
- `Foo*` 可直接用作 `extern func` 的参数类型、返回类型以及 `@abi type` 的成员字段类型。
- 当前版本中,`@abi func` 仅适用于顶层 `func`; 方法、lambda、闭包、绑定方法值都不是合法的 `@abi func`。
- 顶层 `@abi func` 若要进入 ABI 边界,其全部参数与返回值必须 ABI 兼容; 其中无字段对象形式 `@abi type` 只能以 `T*` 形态出现。
- 顶层 `@abi func` 仍然是 Feng 自己实现的函数; 公开或非公开形态都可作为 ABI 回调函数来源。仅顶层 `open @abi func` 在语义上保留为未来面向其他语言的 C ABI 导出 surface。
- 公开 `@abi` 接口未来可作为头文件与导出清单的 surface 来源; 具体产物格式与生成流程由构建与包分发规范单独定义。
- `@abi func` 内部若可能抛出异常,必须在函数体内捕获并转换为 C 侧可理解的返回约定; 未捕获异常不得穿越 ABI 边界传播。

```feng
@abi
spec CmpFunc(a: int, b: int): int;

@abi
func int_cmp(a: int, b: int): int {
    return a - b;
}

@abi
open func point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}
```

## 6 一元 `&` 与指针来源

### 6.1 允许取址的对象

仅允许对以下六类值取 C 指针:

1. 基本标量
2. `enum`
3. 声明了字段的 `@abi` 对象
4. `string`
5. ABI 兼容数组
6. 标注 `@abi` 且通过 ABI 检查的顶层 `func`（目标类型必须显式给出为 `Foo*`）

其它类型一律报错,不做隐式兜底。

诊断要求:

- 对不在上述允许集合中的类型执行一元 `&` 时,编译器必须明确诊断“该类型不支持形成 ABI 指针”或等价含义,不得误报为阶段性“暂不支持”实现缺口。

### 6.2 返回指针含义

1. `&scalar`: 返回该基本标量存储单元的首地址指针,类型 `T*`。
2. `&enum_value`: 返回该 enum 值存储单元的首地址指针,类型 `T*`; 其 ABI 内存表示与 `int` 相同。
3. `&abi_value`: 返回该声明了字段的 `@abi` 值对应 ABI payload 的首地址指针,类型 `T*`。
4. `&str`: 返回 `string` 的 ABI 兼容数据地址指针,类型 `string*`; 当前 `c` 目标下该地址为 UTF-8 数据区首地址。
5. `&arr`: 返回 ABI 兼容数组第 `0` 个元素地址; 空数组返回 `0` 指针,类型 `T*`。
6. `&abi_fn`: 在目标类型显式给出为 `Foo*` 时,返回该顶层 `@abi func` 的函数指针。

补充规则:

- 无字段对象形式 `@abi type` 不能通过一元 `&` 形成 `T*`。
- 这类 `T*` 仅可来自 `extern func` 返回值、外部 ABI 边界传入值以及已有同类型指针的继续传递。

### 6.3 可写性与生命周期

1. `&string` 结果不在语言层强制只读; 是否允许写入由调用方声明的 C 契约决定。若写入破坏 Feng `string` 约束,行为未定义。
2. `&array` 是否可写由数组可写层语义决定。
3. `&abi_value` 是否可写由绑定可变性与成员可写规则共同决定; 该规则只适用于声明了字段的 `@abi type`。
4. `&scalar` 与 `&enum_value` 是否可写由绑定的可变性决定。
5. 数据指针默认借用,仅保证调用期间有效; C 侧若可能缓存、异步使用或以其他方式逃逸使用,开发者必须显式保活 owner。
6. 函数指针 `Foo*` 本身无生命周期问题,但与之配套传递的 `user_data` 等附带对象仍由调用方负责保活。
7. 允许对临时值取数据指针; 编译器不做自动延寿。若调用返回后仍需继续使用,开发者必须自行保活 owner。

### 6.4 函数指针来源与禁止项

`Foo*` 可来自以下位置:

1. 对满足条件的顶层 `@abi func` 执行 `&` 取址。
2. `extern func` 返回值。
3. 已有 `Foo*` 绑定、参数、字段或返回值的继续传递。

补充规则:

- 对顶层 `@abi func` 执行 `&` 时,目标类型必须显式给出为 `Foo*`; 编译器据此检查该 `func` 与 `Foo` 的参数和返回值完全一致。
- 缺少目标类型、存在重载歧义或 `func` 与 `Foo` 签名不一致时,编译器必须报错。
- 以下所有情况都禁止作为 `Foo*` 的来源: 未标注 `@abi` 的顶层函数、成员函数、lambda、闭包、绑定方法值以及任何带环境捕获的 callable 值。
- 除 `&abi_fn` 场景外,编译器通常无法追溯 `Foo*` 的真实来源是否匹配 `Foo` 签名; 这类 ABI 正确性由开发者通过 `extern func` 声明或外部 API 契约保证。

### 6.5 指针类型安全

- 一元 `&` 的结果类型为 `T*` 或 `Foo*`,与取址目标相匹配。
- 指针在 Feng 中只可用于 ABI 边界的存储、传递和返回,不作为通用编程能力开放。
- 仅允许同类型指针参与 `==` / `!=`,结果按原生指针地址身份判定; 不支持 `<` / `<=` / `>` / `>=` 之类的顺序比较。
- 凡超出上述边界的解引用、调用、运算或显式转换,编译期都必须报错。
- 二元 `&` 继续保留为按位与,一元 `&` 仅表示取地址,两者按语法位置区分。

```feng
@cdecl("c_use_i32_ptr")
extern func c_use_i32_ptr(p: int*): void;

let x: int = 42;
let p: int* = &x;
c_use_i32_ptr(p);

@abi
spec PointOperate(p: Point): void;

@abi
func handle_point(p: Point) {
    print(p.x, p.y);
}

let cb: PointOperate* = &handle_point;

@cdecl("point")
extern func run_point_operate(p: Point, cb: PointOperate*): void;
```

## 7 `string` 的 ABI 0拷贝规则

`string` 在 ABI 边界只定义借用与 0 拷贝能力; 当 ABI 位写成 `string*` 时,表示该位承载 `string` 的 ABI 兼容数据地址; 当前 `c` 目标下该地址为 UTF-8 数据区首地址。

规则说明:

- `extern func` 若需要字符串数据指针,必须在签名中显式声明对应指针类型（如 `string*`）,并在调用点显式取址（如 `&str`）; 不允许把 `string` 作为入参自动转换为指针。
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
- 当 ABI 位写成 `T[]*` 时,其 C surface 按 `T*` 处理,指向数组第 `0` 个元素; 长度仍需由独立字段或独立参数显式表达。
- ABI 兼容数组的长度不跟随 `&` 一起隐式传递,长度必须作为显式字段或显式参数表达。
- 数组自身不属于 `@abi type` 可直接内联的字段类型; 需要在 `@abi type` 中表达数组数据时,必须改用显式数据指针与长度字段。
- ABI 数组使用点无需再次递归遍历成员,只查询元素类型 `T` 是否已通过当前 ABI 资格检查。

## 9 开发者责任与诊断

开发者必须根据调用方声明的 C 契约判断是否需要延长引用寿命:

1. 同步只读调用: 可用默认借用与 0 拷贝。
2. 可能逃逸: 必须显式保活 owner（例如 wrapper 成员持有原始 `string`、数组或 `@abi` 对象）。
3. 不允许把“缓存裸指针”误当作“已保活对象”。
4. `extern func` 的参数位或返回位若写成 `Foo*`,其 ABI 真实性由开发者保证,编译器不证明外部实现与声明完全一致。

推荐诊断风格示例:

- `type BufferList` 含有不能直接内联到 `@abi type` 的字段 `items`,因此不能标记为 `@abi`。
- `func point_sum` 的参数 `user` 类型不是 ABI 兼容类型,因此该函数不能标记为 `@abi`。
- `spec PointHandler` 是对象形式的 `spec`,不能标记为 `@abi`; `@abi` 的 `spec` 仅适用于 callable-form。
- `let cb = &int_cmp` 缺少显式目标 `Foo*` 类型,因此不能确定函数指针类型。

## 10 C互操作完整示例

```feng
open module libc.math;

@abi
type Point {
    var x: int;
    var y: int;
}

@abi
spec PointOperate(p: Point): void;

let point_lib = "point";

@cdecl(point_lib)
extern func point_distance(p1: Point, p2: Point): float;

@cdecl(point_lib)
extern func run_point_operate(p: Point, cb: PointOperate*): void;

@abi
func handle_point(p: Point) {
    print("Point:x=", p.x, " y=", p.y);
}

@abi
open func point_sum(p1: Point, p2: Point): Point {
    return Point {
        x: p1.x + p2.x,
        y: p1.y + p2.y,
    };
}

func main(args: string[]) {
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
- 本文档: C 库来源与调用方式、ABI 兼容资格、`@abi`、`Foo*`、`extern func` 导入声明、导出函数和回调规则的独立补充文档。
