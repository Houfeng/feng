# FC语言核心规范(最终完整版)

## 1 设计哲学

FC是一门**强类型、静态类型、支持鸭子类型(鸭型)**的编程语言,秉持极简主义设计理念,语法紧凑、关键字稀少,兼顾脚本语言的简洁性与 C 语言的高效性; 支持闭包、函数式编程与结构化异常处理,全程保障内存安全; 以 `type` 统一定义所有类型,通过 `extern` 标记实现与 C 语言的安全互操作,底层可无缝映射为 C 语言代码,无额外运行时开销; 搭配全自动 GC 实现无感知内存管理,支持 C 标准库、第三方 C 库的结构体和函数指针双向互操作,编译器通过 `extern` 标记完成编译期 ABI 与类型安全检查; 同时支持 FC 自有闭源二进制包分发和 C ABI 兼容包分发,兼顾源码保护、编译加速与跨语言复用。

## 2 语言核心特性

- 静态类型: 变量、参数、返回值类型在编译期确定,运行期不可更改。
- 强类型: 严格类型检查,禁止隐式类型转换,类型不匹配直接编译报错。
- 无 `any` 类型: 不提供动态任意类型,所有变量、参数均为明确类型。
- 鸭子类型(鸭型): 类型无需显式继承或实现,对象方法或函数签名一致即可兼容。
- 支持闭包: `Lambda` 表达式可捕获外部作用域变量,延长变量生命周期。
- 类型推导: 变量有初始值时,可省略类型标注,编译器自动推导。
- 函数返回值: `void` 代表空无类型,无返回值时可省略,有返回值可自动推导类型。
- 参数规则: 所有参数必须显式标注类型,不可省略或推导。
- 类型使用规范: 所有类型必须先定义、后引用,禁止嵌套、匿名或内联定义。
- 数组支持: 采用 `T[]` 语法,支持一维及多维数组,如 `T[][]`、`T[][][]`。
- 异常处理: 支持完整 `try/catch/finally` 结构化异常处理。
- 默认零值: 未初始化变量由编译器自动赋予对应类型默认零值。
- 内存管理: 全自动 GC,无指针、无手动内存分配或释放。
- C 互操作核心: `extern type` 定义可与 C 互通的类型,`extern fn` 声明 C 外部函数或定义可传 C 的 FC 回调,`link` 负责链接 C 库,全程进行编译期安全检查。
- 包分发特性: 支持 FC 自有二进制包(非 `extern` 公有成员)和 C ABI 兼容包双分发模式,支持闭源复用与编译加速。

## 3 基础语法规则

1. 文件扩展名: `.fc`
2. 语句结束符: 分号 `;`
3. 代码块: 统一使用花括号 `{}` 包裹
4. 注释规范:
   - 单行注释: `// 注释内容`
   - 多行注释: `/* 注释内容 */`
5. 代码书写顺序: 模块声明 → 链接指令 → 模块导入 → 类型定义 → 函数/业务代码,不可颠倒

## 4 核心关键字

| 关键字 | 含义 |
| --- | --- |
| `type` | 定义 FC 原生内部类型,包括对象类型和函数类型。 |
| `extern type` | 定义可与 C 语言互通的类型,包括 C 兼容结构体和 C 函数指针类型。 |
| `fn` | 定义 FC 内部普通函数、成员方法和构造函数。 |
| `extern fn` | 声明 C 语言实现的外部函数; 定义可作为函数指针传给 C 的 FC 回调函数; 定义 C ABI 兼容导出函数。 |
| `var` | 修饰可变变量、成员、参数。 |
| `let` | 修饰只读变量、成员。 |
| `pu` | 设置公开访问权限。 |
| `pr` | 设置私有访问权限。 |
| `self` | 类型内部引用自身,等价于 `this`。 |
| `mod` | 文件级模块声明。 |
| `use` | 导入外部公开模块或二进制包。 |
| `link` | 链接 C 标准库、第三方 C 库或 FC 二进制编译产物。 |

## 5 模块系统

### 5.1 模块声明规则

- `mod` 必须写在文件最顶部,无花括号、无代码块。
- 一个文件仅归属一个模块,多文件可声明同一模块,编译后自动合并。
- 模块名支持 `.` 分隔多级命名空间。
- 访问权限: `pu mod` 声明公开模块,可被外部导入; `mod` 默认私有模块。

### 5.2 模块导入规则

- `use` 必须位于 `link` 指令之后、类型与函数定义之前。
- 调用外部成员格式: `模块名.成员名`
- 支持导入源码模块、FC 自有二进制包、C ABI 兼容包。

```fc
pu mod my.app.user;
use my.utils;
```

## 6 C库链接与互操作规范

### 6.1 C库链接指令(link)

使用 `link "库名/路径"` 指令链接 C 库或 FC 编译二进制库,位于模块声明之后、模块导入之前。编译器自动识别链接类型,支持三种形式:

1. 系统库名: 无特殊路径前缀,编译器自动补全系统库前缀和后缀
2. 相对路径: 以 `./` 或 `../` 开头,相对于当前 `.fc` 文件路径
3. 绝对路径: 以 `/` 开头,直接指定库文件完整路径

```fc
// 链接系统数学库
link "m";

// 链接当前目录自定义 C 库
link "./libtest.so";

// 链接绝对路径第三方 C 库
link "/usr/local/lib/libcurl.so";
```

### 6.2 C兼容结构体定义(extern type)

使用 `extern type` 定义可与 C 直接映射的结构体,无需 C 侧额外声明。其内存布局、字节序、内存对齐与 C 语言完全一致,可直接在 FC 与 C 之间按值或按指针传递,并直接读写成员。

语法规则:

- 仅允许声明成员变量,禁止添加构造函数、成员方法
- 成员仅支持: 基础类型、其他 `extern type` 类型、固定长度数组
- 成员需用 `var` 或 `let` 修饰,遵循 FC 变量声明规范
- FC 的 GC 不管理该类型内存,需通过 C 函数或手动释放
- 编译器自动校验与 C 的内存兼容性

定义示例:

```fc
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

### 6.3 C函数指针类型定义(extern type)

使用 `extern type` 定义与 C 函数指针完全兼容的函数类型,用于 C 回调函数传递,签名需与 C 侧完全一致。

```fc
// 定义 C 兼容的比较函数指针类型
extern type CmpFunc(a: int, b: int): int;

// 定义 C 兼容的结构体回调函数类型
extern type PointCallback(p: Point);
```

### 6.4 C外部函数声明(extern fn)

使用 `extern fn` 声明 C 语言实现的外部函数,仅声明函数签名、无函数体,用于调用 C 库函数。参数和返回值需为 C 兼容类型,如基础类型、`extern type` 类型和指针。

```fc
// 声明 C 标准库数学函数
extern fn sin(x: float): float;
extern fn sqrt(x: float): float;

// 声明操作 C 兼容结构体的 C 函数
extern fn create_point(x: int, y: int): Point;
extern fn set_point_callback(cb: PointCallback);
```

### 6.5 FC回调函数定义(extern fn)

使用 `extern fn` 定义可作为函数指针传给 C 的 FC 回调函数。此类函数有函数体,编译器自动生成 C 兼容 ABI,并进行强制编译期检查。

约束规则:

- 禁止捕获外部变量
- 禁止使用闭包
- 仅支持 C 兼容类型作为参数和返回值
- 不可使用 FC GC 托管类型,如 `string`、动态数组、原生 `type` 对象

```fc
// 可传给 C 的比较回调函数
extern fn my_int_cmp(a: int, b: int): int {
    return a - b;
}

// 可传给 C 的结构体回调函数
extern fn my_point_handle(p: Point) {
    print(p.x, p.y);
}
```

### 6.6 C互操作完整示例

```fc
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

// 定义 FC 回调函数
extern fn handle_point(p: Point) {
    print("Point:x=", p.x, " y=", p.y);
}

// FC 内部普通函数
fn main(args: string[]) {
    // 初始化 C 兼容结构体
    let p1 = Point {x: 10, y: 20};
    let p2 = Point {x: 30, y: 40};

    // 调用 C 函数
    let dis = point_distance(p1, p2);
    print(dis);

    // 传递 FC 回调给 C
    run_point_operate(p1, handle_point);
}
```

## 7 类型系统

FC 的类型系统采用强类型、静态类型设计,统一使用 `type` 和 `extern type` 描述 FC 原生类型与 C 兼容类型,并支持类型推导、鸭子类型(鸭型)、默认零值初始化与数组类型。

本规范仅保留类型系统概要说明,详细的数据类型、变量声明、函数类型、自定义类型和鸭型规则已拆分到独立文档: [FC语言类型系统规范](./feng-type.md)。

类型系统要点:

- 基础类型包含整数、浮点、`bool`、`string`、数组和 C 互操作指针类型,其中 `i64` 的别名是 `int`,`f64` 的别名是 `float`。
- 有初始值的变量支持类型推导,无初始值变量使用默认零值初始化。
- FC 原生函数类型与 C 兼容函数指针类型分离设计。
- FC 原生 `type` 支持成员、构造函数、方法和访问控制。
- 鸭子类型兼容性由编译器在编译期检查。

## 8 函数定义规则

### 8.1 FC内部普通函数

仅用于 FC 内部,无 `extern` 修饰,支持闭包与变量捕获,可使用所有 FC 类型。

规则说明:

- 参数必须显式标注类型,默认只读不可修改; 使用 `var` 可声明可变参数
- `void` 代表空无类型; 无返回值时可省略不写,有返回值时可自动推导,也可显式声明
- 函数类型必须先定义,再作为参数或返回值使用

```fc
// 无返回值,void 省略
fn test(x: int, var y: int) {
    y = y + 1;
}

// 自动推导返回 int 类型
fn add(a: int, b: int) {
    return a + b;
}
```

### 8.2 程序入口函数

程序唯一入口为 `main` 函数,必须接收 `string[]` 类型的 `args` 命令行参数,无需 `extern` 修饰。

```fc
fn main(args: string[]) {
    // 程序执行入口
}
```

## 9 Lambda表达式与闭包

- `Lambda` 不是独立的函数类型定义,而是函数实现或函数字面量的一种简写形式; 其可用位置和可赋值性仍由目标函数类型决定
- 仅单行 `Lambda` 使用 `->` 连接参数与表达式,省略 `{}` 和 `return`
- `Lambda` 参数必须显式标注类型
- 支持捕获外部作用域变量,形成闭包,GC 自动管理闭包内存
- 闭包禁止加 `extern`,不可用于 C 函数指针互操作

```fc
// 单行 Lambda
let func = (x: int) -> x * 2;

// 闭包示例
type IntToInt(x: int): int;

fn make_adder(base: int): IntToInt {
    // 捕获外部变量 base,形成闭包
    return (x: int) -> base + x;
}
```

## 10 数组与多维数组

- 语法: 一维 `T[]`、二维 `T[][]`、三维 `T[][][]`,以此类推
- 数组字面量使用 `[]` 包裹,多维数组采用嵌套写法
- 通过 `数组[下标]` 访问元素,下标从 `0` 开始,越界会触发运行时异常
- 动态数组为 FC GC 托管类型,不可用于 C 互操作

```fc
// 一维数组
let arr = [1, 2, 3];

// 二维数组
let mat = [[1, 2], [3, 4]];

// 数组元素访问
let num = arr[0];
let val = mat[1][1];
```

## 11 流程控制

### 11.1 普通条件判断

```fc
if a > b {
    // 逻辑代码
} else if a == b {
    // 逻辑代码
} else {
    // 逻辑代码
}
```

### 11.2 if模式匹配

可作为表达式赋值,支持分支匹配,默认匹配 `else` 分支。

```fc
let stage = if age {
    0: "婴儿",
    18: "成年",
    60: "老年",
    else: "青年"
};
```

### 11.3 循环语句

支持 `while`、`for` 循环,搭配 `break` 跳出循环,搭配 `continue` 跳过当前循环。

### 11.4 异常处理try/catch/finally

- `try`: 包裹可能触发异常的代码
- `catch`: 捕获异常并执行异常处理逻辑
- `finally`: 无论是否发生异常、是否 `return`,均会执行
- 常用于资源释放和收尾操作

```fc
fn test() {
    try {
        let arr = [1, 2, 3];
        let num = arr[5]; // 数组越界异常
    } catch {
        print("程序发生异常");
    } finally {
        print("执行收尾操作");
    }
}
```

## 12 垃圾回收GC

- 采用全自动垃圾回收机制,无需手动管理内存
- 无 `gc_unsafe`,无指针,无 `malloc/free` 等底层内存操作
- GC 托管对象: 字符串、动态数组、FC 原生 `type` 对象
- 非托管对象: `extern type` 定义的 C 兼容类型、`*T` 指针、C 分配内存,需自行释放

## 13 FC包分发与二进制复用规范

### 13.1 分发模式总览

FC 支持两种标准化包分发模式,适配不同复用场景,均支持闭源、压缩、编译加速,不破坏 FC → C 编译架构:

1. FC 自有二进制包(`.fcp`): 适用于非 `extern`、FC 内部 `pu` 公有成员复用,仅 FC 间调用
2. C ABI 兼容二进制包(`.fcc`): 适用于 `extern` 修饰、跨语言复用成员,兼容 C/C++/Go/Python 等语言调用

### 13.2 FC自有二进制包(.fcp格式)

#### 13.2.1 适用场景

- 非 `extern` 修饰的 `pu type` / `pu fn` 公有成员
- 仅 FC 项目间复用,不跨语言
- 需求: 闭源分发、隐藏源码、加速编译、不依赖 C ABI

#### 13.2.2 包格式定义

- 扩展名: `.fcp` (`FCPackage`)
- 底层: ZIP 兼容压缩归档格式,支持标准解压工具
- 核心: 不含 FC 实现源码,仅含接口描述和多平台预编译静态库

#### 13.2.3 包固定结构

```text
库名-版本.fcp
├── manifest.fcm        // 包元信息清单(文本格式,不可修改)
├── api.fci             // 公有接口描述文件(无实现、无源码)
└── lib/
    ├── linux-x64/      // Linux x64 静态库
    │   └── libxxx.a
    ├── windows-x64/    // Windows x64 静态库
    │   └── xxx.lib
    └── macos-arm64/    // macOS arm64 静态库
        └── libxxx.a
```

#### 13.2.4 清单文件(manifest.fcm)

记录包名、版本、模块名、支持平台和接口路径,编译器自动识别。

```text
name:mylib
version:1.0.0
module:mylib
arch:linux-x64,windows-x64,macos-arm64
api_path:api.fci
```

#### 13.2.5 接口文件(api.fci)

由 FC 编译器自动生成,仅包含公有签名,无任何实现逻辑:

```fc
pu mod mylib;

pu type User {
    pu var name: string;
    pu var age: int;
    pu fn get_info(): string;
}

pu fn add_user(u: User): bool;
fn init();
```

#### 13.2.6 编译与使用流程

1. 发布方: FC 源码 → 编译为 C → 生成 C 静态库 → 自动生成 `api.fci` / `manifest.fcm` → 打包为 `.fcp`
2. 使用方: `use` 模块名导入包 → 编译器读取 `api.fci` 完成类型检查 → 自动链接对应平台静态库
3. 特性: 全程无源码暴露,跳过库源码编译,直接链接加速

#### 13.2.7 约束规则

- 仅支持 FC 间复用,不兼容 C 语言调用
- 接口文件不可手动修改,否则链接失败
- 不同 FC 编译器版本编译的包不兼容
- 私有成员(`pr`)不写入接口文件,完全隐藏

### 13.3 C ABI兼容二进制包(.fcc格式)

#### 13.3.1 适用场景

- `extern type` / `extern fn` 修饰的 C 兼容成员
- 需跨语言(C/C++/Go/Python)复用
- 需生成 C 可调用的动态库或静态库

#### 13.3.2 包格式定义

- 扩展名: `.fcc` (`FCC-ABIPackage`)
- 底层: ZIP 兼容压缩归档格式
- 核心: C 标准 ABI 接口 + 多平台二进制库 + C 兼容头文件(可选)

#### 13.3.3 包固定结构

```text
库名-版本.fcc
├── manifest.fcm        // 包元信息清单
├── api.fci             // FC 侧 C 兼容接口描述
├── include/            // C 语言头文件(可选,供外部调用)
│   └── xxx.h
└── lib/
    ├── linux-x64/
    │   ├── libxxx.a    // 静态库
    │   └── libxxx.so   // 动态库
    ├── windows-x64/
    │   ├── xxx.lib
    │   └── xxx.dll
    └── macos-arm64/
        ├── libxxx.a
        └── libxxx.dylib
```

#### 13.3.4 编译与使用流程

1. 发布方: FC `extern` 接口 → 编译为 C ABI 兼容代码 → 生成 C 静态库/动态库 → 打包为 `.fcc`
2. FC 使用方: `use` 模块名 + `link` 库名,直接调用 `extern` 接口
3. 外部语言使用方: 链接库文件 + 引入头文件,按 C 标准方式调用

#### 13.3.5 约束规则

- 仅支持 C 兼容类型,禁止使用 GC 托管类型,如 `string` 和动态数组
- `extern` 函数禁止捕获变量、使用闭包
- 遵循 C 语言内存布局与调用约定
- 支持动态库热更新、静态库全量嵌入

### 13.4 包导入与链接语法

```fc
// 导入 FC 自有二进制包(.fcp)
use mylib;

// 导入 C ABI 兼容包(.fcc)并链接库
use c_mylib;
link "./c_mylib";
```

## 14 完整代码示例

### 14.1 c_interop.fc

```fc
pu mod libc.interop;
link "m";

// C 兼容结构体
extern type Point {
    var x: int;
    var y: int;
}

// C 兼容函数指针类型
extern type PointCB(p: Point);

// 声明 C 外部函数
extern fn point_add(p1: Point, p2: Point): Point;
extern fn exec_point_cb(p: Point, cb: PointCB);

// 定义 FC 回调函数
extern fn on_point(p: Point) {
    print("Point:x=", p.x, " y=", p.y);
}
```

### 14.2 main.fc

```fc
mod main;
use libc.interop;
use mylib; // 导入 FC 自有二进制包

// FC 内部函数类型
type IntToInt(x: int): int;

// FC 内部普通函数
fn make_adder(base: int): IntToInt {
    return (x: int) -> base + x;
}

fn main(args: string[]) {
    // 操作 C 兼容结构体
    let p1 = Point {x: 10, y: 20};
    let p2 = Point {x: 5, y: 5};
    let res_p = point_add(p1, p2);
    print(res_p.x, res_p.y);

    // 传递 FC 回调给 C
    exec_point_cb(res_p, on_point);

    // 调用 FC 自有包成员
    let user = mylib.User{name:"test", age:20};
    mylib.add_user(user);

    // 闭包调用
    let add5 = make_adder(5);
    print(add5(3));

    // 数组操作
    let arr = [10, 20, 30];
    print(arr[0]);

    // 异常处理
    try {
        let num = arr[10];
    } catch {
        print("数组越界异常");
    } finally {
        print("程序执行完毕");
    }
}
```

## 15 核心语法约束

### 15.1 语言与类型约束

- 强类型、静态类型,无隐式类型转换。
- 支持鸭子类型(鸭型)、`Lambda` 与闭包,闭包不可用于 C 互操作。
- 类型系统统一采用 `type` / `extern type` 体系,详细规则见 [feng-type.md](./feng-type.md)。
- 支持 `try/catch/finally` 结构化异常处理。
- 模块声明在顶部,`link` 指令紧随其后,代码顺序固定。

### 15.2 C互操作与分发约束

- `extern type`: 定义 C 兼容结构体和 C 函数指针类型
- `extern fn`: 声明 C 外部函数或定义可传 C 的 FC 回调
- 编译器对 `extern` 标记内容做 C ABI 编译期检查
- FC 普通 `fn` 与 C ABI 完全隔离,保障安全
- 程序入口 `main` 带 `string[] args` 参数,无需 `extern`
- 全自动 GC,不管理 C 兼容类型内存
- 支持 `.fcp`(FC 自有包)和 `.fcc`(C ABI 包)双分发模式
- 二进制包不含源码,支持闭源复用与编译加速
- `.fcp` 包仅 FC 间复用,`.fcc` 包支持跨语言复用

> 注: 文档部分内容可能由 AI 生成。
