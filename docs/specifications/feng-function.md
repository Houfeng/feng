# Feng 语言函数规范

本文档补充 [feng-language.md](./feng-language.md) 中的函数系统概要,聚焦 `func`、程序入口、Lambda 与闭包规则。可调用形状的声明见 [feng-spec.md](./feng-spec.md),参数绑定细节见 [feng-binding.md](./feng-binding.md)。

## 1 职责

- 函数用于封装可执行逻辑,对外提供可调用行为。
- 函数通过参数与返回值在调用方和实现体之间传递输入输出。
- 函数承担模块级行为、对象行为，以及 `type` 特殊成员中的实例构造与终结清理行为,分别体现为顶层函数、成员方法、构造函数与终结器函数；后二者的规则细节见 [Feng 语言类型规范](./feng-type.md)。
- 标注 `@mixable` 的静态方法可以通过成员展开生成普通静态与实例 wrapper，以复用行为
  而不复制方法体或建立继承关系。
- 函数可作为闭包或方法值参与表达式求值,并与 `spec`、`type`、`binding` 规则协同工作。

## 2 术语

- 顶层函数: 声明在模块级顶层的 `func`。
- 成员方法: 声明在 `type` 内部的 `func`。
- 构造函数: `type` 的特殊成员 `func`,用于实例初始化; 声明与约束细节见 [Feng 语言类型规范](./feng-type.md)。
- 终结器函数: `type` 的特殊成员 `func`,用于对象释放前的清理; 声明与约束细节见 [Feng 语言类型规范](./feng-type.md)。
- 入口函数: 程序唯一入口 `main(args: string[])`。
- 可调用形状: 由 `spec Name(args): ReturnType;` 声明的可调用契约。
- Lambda: 函数实现或函数字面量的简写形式,不是独立的可调用形状声明,也不产生匿名函数类型; 单表达式 Lambda 使用 `->`,多行 Lambda 直接使用块体。
- 闭包: 捕获外部作用域变量的函数值。
- 方法值: 以 `obj.method` 形式取出的、绑定接收者的实例方法 callable value，或以
  `Type.method` 形式取出的、不带接收者的静态方法 callable value。
- 可复用静态方法: 标注无参数内建注解 `@mixable`、以 object-form `spec` 作为第一个
  参数，并可通过成员展开传播的静态方法。
- 静态 wrapper: 成员展开为目标生成的、保留来源完整签名并完整限定调用来源静态方法
  的静态方法。
- 实例 wrapper: 从类型最终保留的 `@mixable` 静态方法统一派生的普通实例方法；它
  删除静态方法第一个参数，并把当前实例作为该参数传入当前类型的静态入口。

## 3 语法

正确语法一,顶层函数、成员方法与构造函数:

```feng
open func add(a: int, b: int): int {
    return a + b;
}

type User {
    let name: string;

    func User(name: string) {
        self.name = name;
    }

    open func info(): string {
        return self.name;
    }
}
```

正确语法二,可调用形状、单行 Lambda 与入口函数:

```feng
spec IntMapper(x: int): int;

func make_adder(base: int): IntMapper {
    return (x: int) -> base + x;
}

func main(args: string[]) {
    let add10: IntMapper = make_adder(10);
    print(add10(2));
}
```

正确语法三,多行 Lambda、捕获外层 `self` 与方法值:

```feng
spec M0(): void;

type User {
    func say() {}
    func say(msg: string) {}

    func binder(): M0 {
        return () {
            self.say();
        };
    }
}

func test(m: M0) {
    m();
}

let u = User {};
test(u.say);
let s_ok: M0 = u.say;
```

正确语法四,`void` 省略、返回类型推导与显式 `let` 参数:

```feng
func touch(x: int, var y: int) {
    y = y + 1;
}

func add(a: int, b: int) {
    return a + b;
}

func visit(let name: string) {
    print(name);
}
```

错语法一,参数省略类型:

```feng
func sum(a, b: int): int {
    return a + b;
}
```

错语法二,仅以返回值区分重载:

```feng
func parse(x: int): int {
    return x;
}

func parse(x: int): string {
    return "ok";
}
```

错语法三,多行 Lambda 仍使用 `->`:

```feng
let add: AddSpec = (x: int) -> {
    let y = x + 1;
    return y;
};
```

错语法四,未显式标注类型的绑定无法承载重载方法值:

```feng
type User {
    func say() {}
    func say(msg: string) {}
}

let u = User {};
let s = u.say; // 错误: say 的重载本身合法,但 let s 无法推导唯一目标可调用形状; 应显式标注绑定类型
```

正确语法四-b，显式闭合泛型函数值和方法值:

```feng
spec ReadInt(): int;

func read<T>(): T {
    // 示例主体省略
}

type Reader {
    func read<T>(): T {
        // 示例主体省略
    }
}

let topReader: ReadInt = read<int>;
let owner = Reader();
let methodReader: ReadInt = owner.read<int>;
let convertedMethodReader = (ReadInt)owner.read<int>;
```

正确语法四-c，具体类型与可见 `fit` 的静态方法值：

```feng
spec IntMapper(value: int): int;

type Math {
    static func double(value: int): int {
        return value * 2;
    }
}

type ExtendedMath {}

fit ExtendedMath {
    static func triple(value: int): int {
        return value * 3;
    }
}

let double: IntMapper = Math.double;
let triple: IntMapper = ExtendedMath.triple;
let converted = (IntMapper)Math.double;
```

正确语法五，`@mixable` 静态方法:

```feng
spec Widget {
  func draw(area: Area): void;
}

type View: Widget {
  @mixable
  open static func draw(target: Widget, area: Area): void {
    // 可复用绘制逻辑
  }
}

type Button: Widget {
  ...: View;
}
```

`@mixable` 静态方法也可以显式声明为 `seal`。它仍是私有成员，但直接展开其来源的
目标 type 可按 §4.3 定义的受限授权生成 wrapper 并调用该方法：

```feng
type ProtectedView: Widget {
  @mixable
  seal static func draw(target: Widget, area: Area): void {
    // 只供直接展开 ProtectedView 的目标复用
  }
}
```

错语法五，`@mixable` 带参数、标注实例方法或没有目标参数:

```feng
type Bad: Widget {
  @mixable("draw")
  static func a(target: Widget): void {} // 错误: @mixable 不接受参数

  @mixable
  func b(): void {}                      // 错误: 只能标注静态方法

  @mixable
  static func c(): void {}               // 错误: 缺少第一个 object-form spec 参数
}
```

## 4 语义

### 4.1 通用语义

- `func` 用于提供可执行实现; 可调用形状由 `spec` 定义,而不是由 `func` 或 `Lambda` 单独声明。
- Lambda 作为值参与绑定、参数传递或返回时,必须由显式 callable-form `spec` 提供目标类型: 绑定位置需要显式绑定类型,参数位置需要形参类型为 callable-form `spec`,返回位置需要当前函数显式声明 callable-form `spec` 返回类型。目标为泛型 callable-form `spec` 实例时,必须先用目标实例的类型实参替换其参数与返回类型,再检查 Lambda 或 `func` 的签名贴合。`return (x: int, y: int) -> x + y;` 是合法写法,前提是当前函数的返回类型已经显式声明为匹配的 callable-form `spec`; 函数省略返回类型时不得通过返回 Lambda 推导匿名函数类型。
- 顶层函数、成员方法、构造函数与终结器函数都使用 `func` 进入函数系统,但构造函数与终结器函数作为 `type` 特殊成员,其声明与补充约束统一见 [Feng 语言类型规范](./feng-type.md)。
- `void` 表示无返回值; 无返回值的顶层函数、普通成员方法与 Lambda 可省略 `: void`。未显式声明返回类型时,返回类型按以下规则推导: 无 `return` 语句,或所有 `return` 均不带表达式（`return;`）则推导为 `void`; 所有 `return` 路径类型一致则推导为该类型; 多条 `return` 路径类型不一致则编译期报错。有返回值函数也可显式声明返回类型，显式声明时编译器将所有 `return` 表达式与应声明类型进行对照检查。块体 Lambda 中的 `return` 只属于该 Lambda 自身的 callable body，并按 Lambda 的目标可调用形状或推导返回类型检查；外层 callable 的返回约束不适用于该 Lambda。构造函数与终结器函数自身不直接适用本条的声明细节,相关规则见 [Feng 语言类型规范](./feng-type.md)。
- 所有参数都必须显式标注类型; 参数上的 `let`/`var` 可省略,省略时默认按 `let` 处理。
- 顶层 `func` 默认等价于 `seal func`,需要跨模块暴露时必须显式写为 `open func`; 成员方法可见性规则见 [Feng 语言类型规范](./feng-type.md)。
- `Lambda` 支持捕获外部作用域变量形成闭包; 闭包对外部绑定的捕获是引用捕获——被捕获的 `var` 绑定与外层作用域共享同一存储,闭包内对该绑定的赋值对外层可见; 多个闭包捕获同一 `var` 时互相共享该绑定; 被捕获的 `let` 绑定在闭包内仍不可重新赋值。闭包可用位置与可赋值性由目标可调用形状决定。
- 单表达式 Lambda 使用 `->` 连接参数列表与表达式结果; 多行 Lambda 直接使用块体,不使用 `->`。
- 参数可显式写为 `let` 参数; 省略 `let`/`var` 时只是默认按 `let` 处理,并不禁止显式书写 `let`。
- 参数传递中的数值类型匹配遵循 [Feng 内建类型规范](./feng-builtin-type.md): 数值字面量与纯字面量数值常量表达式可按已确定目标数值类型贴合; 已具备静态类型的绑定值在不同数值类型间传递必须显式转换。
- `extern func` 可以声明类型参数,但仅当其每个参数位与返回位在抹除类型参数后都对应唯一且固定的外部调用 surface 时才合法; 若某一位置会因具体类型实参而改变外部 surface,或需要额外的类型描述符、wrapper 或按实例分化的导入签名,则该声明非法。调用这类泛型 `extern func` 时,既可以显式提供类型实参,也可以在类型实参能由实参类型唯一确定时省略; 若无法唯一确定,则编译期报错。当前这条“按包裹形状递归推导”的省略规则只适用于泛型 `extern func` 调用,不改变普通函数、方法或构造函数既有的泛型调用匹配规则。
- `self` 只由定义于 `type` 或 `fit` 块中的实例成员方法，以及 `type` 的构造函数和终结器引入; Lambda 本身不会声明新的 `self`,但可像捕获其他外层绑定一样捕获所在实例成员方法、构造函数或终结器作用域中的 `self`。
- `type` 字段的初始化器表达式本身不得直接引用 `self`; 但当字段类型解析为 `spec` 定义的可调用形状,且初始化器是 Lambda 时,该 Lambda 体可捕获外层 `type` 的 `self`(捕获在对象构造完成后才生效)。
- 方法值在形成时绑定当前接收者：值类型接收者按值捕获，后续调用中的 `self` 引用方法值保存的值存储；引用类型接收者复制引用，后续调用中的 `self` 引用该引用所指向的同一实例。调用方法值本身不再复制或重新选择接收者。若方法存在重载，仍需由上下文唯一确定目标可调用形状。
- 尚未绑定到 callable-form `spec` 的顶层函数或实例方法引用，无论当前名称下只有一个
  候选还是存在重载，都必须在明确的 callable-form `spec` 目标中形成值；
  `let callable = function` 和 `let callable = value.method` 不推导匿名 callable 类型。
- object-form `spec` 值作为方法值接收者时，形成点必须同时固定当前 subject 与当前
  witness 视角。来源 requirement 按直接调用使用的成员闭包、可见性过滤和目标可调用
  形状唯一确定；形成后重新赋值原 spec 绑定不得改变已经保存的 subject、witness 或
  requirement。后续调用直接使用形成点保存的 witness 槽，不重新求值接收者，也不重新
  搜索成员或执行重载选择。
- `value: T` 的约束为 object-form `spec` 时，`value.method` 可以在明确 callable-form
  `spec` 目标下形成方法值。来源 requirement 仍按 `T` 的约束实例选择，但 receiver
  保持 `T`：闭合 `T` 为引用类型时复制并保留同一引用，闭合 `T` 为值类型时复制形成点
  的值。形成方法值不得先把 receiver 转换或装箱为 object-form `spec` 值。
- 具体类型的自有静态方法和当前位置可见的 `fit` 静态方法，可以通过 `Type.method`
  在明确 callable-form `spec` 目标下形成静态方法值。候选面、可见性过滤、owner/fit
  泛型代入和重载选择与同一 `Type.method(args)` 直接调用一致；形成点固定唯一的 owner、
  fit 与方法声明，不求值或保存 receiver、subject、witness，也不引入 `self`。
- `T` 受 object-form `spec` 约束时，`T.method` 可以在明确 callable-form `spec` 目标下
  形成静态方法值。来源 requirement 必须按同一 `T.method(args)` 直接调用使用的约束实例、
  父闭包投影、访问过滤和签名规则唯一确定；形成点绑定闭合 `T` 的 type descriptor 中该
  requirement 对应的 witness 槽，但不绑定 receiver 或 subject。后续调用不得重新搜索成员、
  判断满足关系或执行重载选择。
- 对重载函数值或重载方法值,可用于消歧的上下文包括参数位置的目标可调用形状、显式绑定类型与已声明返回类型。
- 声明了函数级或方法级泛参的顶层函数、实例方法或具体静态方法作为值时，必须使用
  `function<TypeArgs...>`、`object.method<TypeArgs...>` 或 `Type.method<TypeArgs...>`
  显式提供完整方法类型实参；目标 callable-form `spec` 不隐式推导来源泛参。泛型 owner
  的类型实参仍由 `Type<OwnerArgs...>.method` 显式给出。类型实参数量和约束检查通过后，
  编译器先闭合来源签名，再按普通未绑定 callable 的规则与目标 callable-form `spec`
  做结构匹配。
- 显式闭合的泛型函数值或方法值仍必须出现在具有明确 callable-form `spec` 目标的绑定、实参或返回位置；`let reader = read<int>;` 不推导匿名 callable 类型。形成后的值是普通闭合 callable，调用形式为 `reader()`，不能再写 `reader<int>()`。
- `read<T>` 或 `self.read<T>` 中的类型实参可以引用当前活动的类型级或函数/方法级泛参；泛型共享体在最终具化点为每组具体实参生成对应的闭合 callable 描述信息。
- callable-form `spec` 的显式转换目标也构成明确的 callable 目标上下文，因此未绑定的
  非泛型函数或方法引用可以直接写为 `(TargetSpec)function`、
  `(TargetSpec)object.method` 或 `(TargetSpec)Type.method`；泛型来源必须先显式闭合。
  转换目标不反向推导来源泛参。

### 4.2 main 函数

顶层 `main` 函数（非 `type` 成员函数）是程序的唯一入口。以下规则仅在编译目标为可直接执行文件（可执行程序）时生效；若编译目标为库，`main` 与普通顶层函数无差别，不会被当作程序入口。

- 一个编译目标为可执行文件的包中，有且只能有一个顶层 `main` 函数。
- `main` 的返回类型固定为 `void`，可省略声明；不得显式声明为非 `void` 类型，否则编译期报错。
- `main` 函数的可见性（`open`/`seal` 或省略）以及其所在 `module` 的可见性，均不影响其作为程序入口的资格。
- 编译器仅将当前包中的顶层 `main` 函数识别为程序入口，不会跨包搜索入口。
- 若编译器在同一包中发现多个顶层 `main` 函数声明，必须在编译期报错并阻止通过。

正确示例,入口函数声明（编译目标为可执行文件）:

```feng
func main(args: string[]) {
    print("hello");
}
```

错误示例,同一包中存在多个顶层 `main`:

```feng
func main(args: string[]) { }  // 第一个 main

func main(args: string[]) { }  // 错误: 同一包中已存在顶层 main，不允许重复声明
```

### 4.3 `@mixable` 静态方法与 wrapper

#### 4.3.1 声明约束

`@mixable` 是不接受参数的内建注解，只能标注具体 `type` 或 `fit Source` 中的静态
方法，或者具体 `type` 中显式声明为 `seal` 的实例字段。字段的合法位置、成员展开、
直接 mix 授权和 `.ft` 语义由
[Feng 语言类型规范](./feng-type.md#4221-mixable-seal-实例字段) 唯一定义；本节以下
规则只适用于静态方法。合法方法必须满足以下约束:

- 至少声明一个参数；
- 第一个参数不是变长参数，且其类型是 object-form `spec`；
- 参数名称没有特殊含义；
- 声明方法的具体类型必须按普通手写 `spec` 使用的当前可见名义关系满足
  第一个参数的 `spec`；如果方法声明在 `fit Source` 中，这里的具体
  类型是 `Source`；
- 方法从来源展开到目标时，目标类型也必须按相同的当前可见名义关系满足该
  `spec`。

上述名义关系与普通手写 `spec` 使用一致，包含类型声明头的直接 `spec`、其传递父
`spec` 及当前可见 `fit` 建立的名义关系。生成 wrapper 前只检查该名义关系，不依赖
当前 `@mixable` 方法或其他生成成员预先证明完整满足。生成字段和 wrapper 进入普通
成员表后，现有 `spec` 满足检查再验证声明类型和目标类型是否真正满足该契约；
生成成员允许参与该最终判定。

除第一个参数约束外，方法的泛型参数与约束、其余普通参数、返回类型、可见性和变长
参数标记继续使用普通静态方法规则。`@mixable` 不表示来源实例、alias、类型链或其他
参数注入，固定传入项只有目标实例。

`@mixable` 不改变成员声明的可见性：省略修饰仍按普通成员规则等价于 `open`，显式
`seal` 的方法仍是 seal 成员。由于 `@mixable seal static` 可以形成跨 type、跨包的
受限 mix 能力，其完整签名必须按所属 type 或 fit 中同位置 open 方法的有效可见范围
检查，包括参数、返回类型、泛型约束及递归组成类型；违反时由提供方使用现有
`AE0327` 报错。未标注 `@mixable` 的普通 seal 方法继续使用类型私有签名范围。

#### 4.3.2 声明类型的实例 wrapper

一个具体类型最终成员面中每个保留的 `@mixable` 静态方法，都无条件派生一个同名普通
实例 wrapper；该规则同样适用于类型显式声明的方法和从其他来源生成的静态 wrapper。
例如 `View` 中的声明概念上派生:

```feng
func draw(area: Area): void {
  View.draw(self, area);
}
```

实例 wrapper 删除静态方法的第一个参数，把当前实例 `self` 按该参数的 object-form
`spec` 视角作为第一个普通实参，并始终完整限定调用当前类型自己的同名静态方法。
实例 wrapper 保留其余签名事实与可见性，作为普通实例方法参与 `spec` 满足、方法值、
重载、冲突、导出和代码生成；它不携带 `mixable` 声明事实，也不参与后续成员展开。

来源静态方法、目标静态 wrapper 和目标实例 wrapper 的可见性固定映射如下：

| 来源静态方法 | 目标静态 wrapper | 目标实例 wrapper |
| --- | --- | --- |
| `@mixable open static` | `@mixable open static` | `open` 实例方法 |
| `@mixable seal static` | `@mixable seal static` | `seal` 实例方法 |

如果 `@mixable` 静态方法声明在 `fit Source` 中，为 `Source` 派生的实例 wrapper
等价于在同一个 `fit` 中手写上述普通实例方法，其归属、可见性、冲突、导出和 `.ft`
恢复使用现有 `fit` 规则。参与某个来源成员面的 `fit Source` 方法集合由
[Feng 语言类型规范](./feng-type.md#422-来源成员面) 唯一定义。

#### 4.3.3 展开目标的静态 wrapper

目标成员展开来源时，来源成员面中每个符合条件的 `@mixable` 静态方法生成一个目标
静态 wrapper。符合条件的方法包括目标位置按普通规则可见的 open 方法，以及由
§4.3.5 直接 mix 授权选中的 seal 方法。该 wrapper:

- 保留来源静态方法的完整签名和可见性；
- 方法体只执行完整限定的来源静态调用；
- 继续携带 `mixable` 声明事实。

例如 `Button` 展开 `View` 后概念生成:

```feng
@mixable
open static func draw(target: Widget, area: Area): void {
  View.draw(target, area);
}

open func draw(area: Area): void {
  Button.draw(self, area);
}
```

第一个方法是目标静态 wrapper；它进入目标最终成员面后，再统一按 §4.3.2 派生第二个
实例 wrapper。静态 wrapper 不复制来源方法体，也不重新绑定来源方法中的任何访问。
来源方法中的完整限定静态成员访问始终属于来源类型；需要动态选择目标行为时，来源
实现必须通过第一个 object-form `spec` 参数调用相应实例能力。

目标生成的静态 wrapper 保留 `mixable` 事实，因此目标以后作为下一层成员展开来源时，
继续生成下一层静态 wrapper 和实例 wrapper。多层调用链与源码可见的完整限定静态
调用链一致，不提供隐式虚方法选择或 `super` 调用。

目标可以显式声明自己的 `@mixable` 静态方法，并通过 `Source.method(target, ...)`
完整限定调用来源逻辑。对一个 `@mixable` 静态方法，其实例投影保留方法名、方法泛型
形状及约束、返回类型、可见性和变长参数事实，删除第一个 object-form `spec` 参数，并
保留其余参数。投影冲突按两个投影已经成为普通实例方法时的声明合法性规则判断；返回
类型不能单独区分投影重载。

把来源 `@mixable` 静态 wrapper 候选加入目标前，目标显式成员按以下规则优先：

- 继续按完整静态成员规则检查候选与目标显式成员；构成冲突时不生成候选；
- 如果目标显式成员也是 `@mixable` 静态方法，还要比较双方的实例投影；投影构成非法
  实例成员冲突时不生成来源候选；
- 目标显式非 `@mixable` 静态方法不参与实例投影检查；目标显式普通实例成员也不参与
  静态 wrapper 候选的跳过判断；
- 每个来源候选独立与目标显式成员比较，因此一个目标显式 `@mixable` 可以使多个投影
  冲突的来源候选跳过。

不同来源产生的静态 wrapper 之间不设来源优先级，也不进行实例投影预筛选，按等价手写
方法的现有规则处理。多个来源最终生成冲突的实例 wrapper 时必须在定义处报错，不得按
来源声明顺序选择或静默去重；实例投影可以合法重载时则全部保留。

实例 wrapper 不适用上述显式成员优先跳过。每个最终保留的 `@mixable` 静态方法都
必须派生实例 wrapper，再由现有普通成员冲突和重载检查决定是否合法；编译器不得静默
省略已保留静态入口对应的实例入口。生成 wrapper 引发后续诊断时，诊断必须保留目标
成员展开声明和来源方法声明的位置映射。

#### 4.3.4 泛型、变长参数与二进制分发

静态 wrapper 保留来源方法的泛型参数、约束、完整参数列表、返回类型、可见性和变长
参数标记；实例 wrapper 只删除第一个参数，其余签名事实保持不变。若最后一个参数是
变长参数，wrapper 必须使用
[Feng 语言变长参数规范](./feng-function-variadic.md) 定义的 `...args` 转发同一个预打包
数组，不得重新分配、复制、遍历或二次打包。

`.ft` 必须记录公开静态方法的 `mixable` 声明事实。同包声明与 `.ft` 恢复声明必须向
普通成员查询提供一致的静态方法签名、泛型、变长参数、可见性和 `mixable` 事实。目标
导出前完成静态与实例 wrapper 生成，并把生成 wrapper 作为目标自身普通成员导出；
生成静态 wrapper 的 `mixable` 事实继续导出，生成实例 wrapper 不携带该事实。

wrapper 只增加源码语义明确的普通静态调用层，不引入运行时方法表、来源实例、方法
环境或动态 receiver 重绑定。

#### 4.3.5 `@mixable seal` 的直接 mix 授权

具体 type 中的以下三种合法成员展开形式都建立相同的直接 mix 关系：

```feng
...: Source;
...: Source = SourceConstruction;
... = SourceConstruction;
```

构造表达式只承担 [Feng 语言类型规范](./feng-type.md) 定义的字段初始化职责，不改变
mixable 候选、wrapper 生成或 seal 授权。一个直接 mix 关系只在以下条件同时成立时，
授予目标 type 对来源方法的受限访问：

1. 当前实现上下文是目标 type 自身的实例方法、静态方法，或编译器为该目标生成的
   mixable wrapper；
2. 被访问方法属于对应 Source 的来源成员面，满足既有 `@mixable` 契约，并同时具有
   `seal`、`static` 和 `mixable` 声明事实；
3. 目标与该 Source 存在上述直接 mix 关系；间接传播关系不授予对原始来源方法的直接
   访问；
4. 方法声明在 `fit Source` 中时，该 fit 必须按普通 fit 可见性规则在当前 mix 位置
   可见。

授权同时用于来源 wrapper 候选选择、生成 wrapper 中的完整限定来源调用，以及目标
自身实例方法或静态方法中的 `Source.method(...)` 显式调用。目标显式成员按既有优先
规则跳过来源 wrapper 时，直接 mix 关系仍然存在，目标仍可显式调用来源实现。

该授权不适用于顶层函数、其他 type、间接 mix 目标或共同实现相同 spec 的 type；
也不扩展来源普通 seal 字段、实例方法、构造函数及未标注
`@mixable` 的 seal static 方法。普通 seal 成员访问、spec/witness 与 fit 访问规则均
保持不变。

生成的 seal 静态 wrapper 保留 `mixable` 事实，因此下一层目标显式直接 mix 当前目标
时可以继续传播；下一层获得的是对当前目标 wrapper 的直接授权，不是对原始来源方法
的间接授权。

#### 4.3.6 跨包声明与链接

当所属 type 或 fit 按现有规则可导出时，package-public `.ft` 除公开方法外，还必须
选择 `seal + static + is_mixable` 方法及生成的同类静态 wrapper，原样记录其 seal、
static、mixable、完整签名、泛型与 reified dependencies 事实。普通 seal 方法及
`seal + !is_mixable` 实例 wrapper 不因本规则进入 package-public 方法面。

`.ft` 中存在该声明不表示普通访问可见：consumer 仍按 seal 拒绝普通成员访问，只能在
§4.3.5 的直接 mix 授权成立时选择和调用。type 来源与当前可见 `fit Source` 来源、同包
AST 与 imported provider 必须进入相同的候选和授权流程。

来源方法及生成的 seal 静态 wrapper 必须具有由 package-public `.ft` 可恢复事实唯一
确定的稳定跨包链接符号。链接可用性不改变 Feng visibility；实现必须复用现有 open
mixable 的泛型、reification 和 wrapper 调用链，不新增运行时动态分派、来源实例、
额外参数或专用 wrapper ABI。

## 5 规则

分为「必须、禁止、建议」。

- [必须] 顶层函数与成员方法使用 `func` 关键字声明实现体; 构造函数与终结器函数作为 `type` 特殊成员,其声明规则见 [Feng 语言类型规范](./feng-type.md)。
- [必须] 构造函数与终结器函数的命名、返回类型及其他声明约束统一遵循 [Feng 语言类型规范](./feng-type.md)。
- [必须] 所有函数参数与 Lambda 参数都必须显式标注类型。
- [必须] `@mixable` 只能无参数标注具体 `type` 或 `fit Source` 中至少带一个参数的静态
  方法，或者具体 `type` 中显式声明为 `seal` 的实例字段；字段规则只引用
  [Feng 语言类型规范](./feng-type.md#4221-mixable-seal-实例字段)。
- [必须] `@mixable` 静态方法的第一个参数必须是非变长 object-form `spec`。
- [必须] `@mixable` 方法的声明类型和展开目标都必须按普通手写 `spec` 使用的当前可见
  名义关系满足第一个参数的 `spec`，完整满足关系由生成成员进入普通成员表后的
  现有检查验证。
- [必须] 每个类型最终保留的 `@mixable` 静态方法都派生删除第一个参数的实例 wrapper，
  且实例 wrapper 必须调用当前类型自己的同名静态方法。
- [必须] 成员展开产生的静态 wrapper 必须完整限定调用来源静态方法并保留 `mixable`
  事实；实例 wrapper 不保留该事实。
- [必须] `@mixable seal static` 的目标静态 wrapper 和实例 wrapper 必须都保留 seal
  可见性；省略成员可见性仍按现有规则等价于 open。
- [必须] 三种合法成员展开形式必须建立相同的直接 mix 授权；仅当当前实现上下文属于
  目标 type、被访问方法同时为 `seal + static + mixable` 且方法属于对应 Source 来源
  成员面时，才允许选择或调用该 seal 方法。
- [禁止] 由间接 mix、相同 module、相同包、来源 type 可见或共同实现 spec 单独获得
  来源 seal mixable 方法访问权。
- [必须] `@mixable seal static` 的完整签名必须按所属 type 或 fit 中同位置 open 方法的
  有效可见范围检查并沿用 `AE0327`；普通 seal 方法的类型私有签名规则不得改变。
- [必须] package-public `.ft` 必须在所属 type 或 fit 可导出时记录
  `seal + static + mixable` 方法及生成的同类静态 wrapper；该记录只表达受限 mix 能力，
  不得使其成为普通公开成员。
- [必须] 来源 `@mixable` 静态 wrapper 候选除完整静态成员冲突检查外，还必须与目标
  显式 `@mixable` 静态方法比较删除双方首参数后的实例投影；投影构成非法普通实例成员
  冲突时，必须由目标显式成员优先并跳过该来源候选。
- [禁止] 对不同 mix 来源增加来源优先级或实例投影预筛选；未由目标显式 `@mixable`
  解决的实例 wrapper 冲突必须交由普通成员规则报告。
- [禁止] 因目标存在显式成员而跳过最终 `@mixable` 静态方法的实例 wrapper；生成后
  必须交由现有普通成员冲突与重载规则处理。
- [必须] 参数省略 `let`/`var` 时默认等价于 `let`; 需要在函数体内修改参数时必须显式使用 `var`。
- [必须] 如需显式声明参数不可变,可以直接写为 `let` 参数。
- [必须] 参数传递时，数值字面量或纯字面量数值常量表达式可按目标数值类型贴合；包含已具备静态类型绑定值的跨类型传递必须显式转换（详见 [Feng 内建类型规范](./feng-builtin-type.md)）。
- [必须] 需要跨模块暴露的顶层函数必须显式标注 `open`; 未显式标注时默认按 `seal` 处理。成员方法可见性规则见 [Feng 语言类型规范](./feng-type.md)。
- [必须] 泛型 `extern func` 仅在每个参数位与返回位都可抹除为唯一且固定的外部调用 surface 时才合法; 若某一位置会随具体类型实参改变外部 surface,或需要额外的类型描述符、wrapper 或按实例分化的导入签名,则必须拒绝该声明。
- [必须] 调用泛型 `extern func` 时,若未显式提供类型实参,则必须仅在可由实参类型唯一推导出全部类型参数时才允许省略; 不能唯一推导时必须报错。该省略规则仅适用于泛型 `extern func` 调用,不得借此扩大普通函数、方法或构造函数的既有泛型匹配范围。
- [必须] 顶层函数与成员方法的重载只按“名称 + 参数个数 + 参数类型”区分,返回值不参与重载; 构造函数的重载规则见 [Feng 语言类型规范](./feng-type.md)。
- [必须] 若两个候选仅返回值不同而参数列表完全相同,必须视为签名冲突。
- [必须] 若同一重载集合中的两个候选在当前可见的显式契约关系下可能同时匹配同一实参类型,必须视为签名冲突。
- [必须] Object-form `spec` 的上下文向上 coercion 属于上述重叠匹配关系；`type T` 与其满足的 `spec S`、子 `spec S1` 与其父 `spec S2` 不得作为同一参数位置的重载区分。编译器不得使用精确匹配优先级、父级距离或调用点显式 cast 消解此类重载集合。
- [必须] 程序入口必须且只能是 `main(args: string[])`，返回类型固定为 `void`，可省略声明但不得声明为非 `void` 类型; 每个包中顶层 `main` 只能有一个; 可见性修饰符不影响入口资格。
- [必须] 单表达式 Lambda 使用 `->`; 多行 Lambda 使用块体且不写 `->`; 块体 Lambda 中的 `return` 归属于该 Lambda 自身，不归属于任何外层 callable。
- [必须] Lambda 作为绑定值、实参或返回值时,必须存在显式 callable-form `spec` 目标类型; 绑定位置来自显式绑定类型,参数位置来自形参类型,返回位置来自当前函数的显式返回类型; 泛型目标必须按目标实例的类型实参完成签名替换后再进行贴合检查。
- [必须] Lambda 可以捕获其外层作用域中的普通绑定,也可以在存在外层 `self` 绑定时捕获该 `self`。
- [必须] 当重载函数或重载方法以函数值形式出现时,必须存在足以唯一确定目标可调用形状的上下文,例如参数位置的目标类型、显式绑定类型或已声明返回类型。
- [必须] 尚未绑定到 callable-form `spec` 的顶层函数或实例方法引用形成值时必须具有
  明确 callable-form `spec` 目标；单一候选同样不得产生匿名 callable 自然类型。
- [必须] 声明函数级或方法级泛参的顶层函数或实例方法作为值时，必须通过显式泛型 target 提供完整类型实参；编译器必须验证实参数量、泛型约束和闭合后的 callable 签名。
- [必须] 显式泛型 target 形成函数值时仍必须具有 callable-form `spec` 目标；形成后的值必须是普通闭合 callable，不得保留可在后续调用重新指定的泛参。
- [必须] object-form `spec` 值的实例方法引用在具有明确 callable-form `spec` 目标时，
  必须按当前 spec 实例完成 requirement 签名替换、访问过滤和重载消歧，并在形成点绑定
  当前 subject 与 witness 视角；原 spec 绑定后续重新赋值不得使已形成的方法值重绑定。
- [必须] object-form `spec` 约束下的泛型值实例方法引用，在具有明确 callable-form
  `spec` 目标时必须复用该约束实例的 requirement 选择，并按闭合 `T` 的值模型绑定
  receiver；不得为形成方法值把 `T` coercion 或装箱为 object-form `spec` 值。
- [必须] 具体类型自有或当前位置可见 `fit` 的静态方法引用，在具有明确 callable-form
  `spec` 目标时，必须复用对应静态直接调用的候选面、访问过滤、泛型代入与重载规则，
  并固定唯一来源；不得伪造 receiver、subject 或 witness。
- [必须] 受 object-form `spec` 约束的类型参数静态方法引用，在具有明确 callable-form
  `spec` 目标时，必须复用对应 `T.method(args)` 直接调用的 requirement 选择，并绑定闭合
  `T` 的 descriptor/witness 槽；不得形成 receiver、subject 或运行时成员查询。
- [必须] callable-form `spec` 的显式转换可以直接以尚未绑定到 spec 的顶层函数、实例
  方法、具体静态方法或受 object-form `spec` 约束的类型参数静态方法引用作为操作数；
  编译器必须以转换目标完成来源选择、结构匹配和 callable value 形成。泛型来源必须先
  通过显式泛型 target 提供完整类型实参。
- [禁止] 通过 callable-form `spec` 目标的参数或返回类型隐式推导并消除来源函数或方法自身声明的泛参。
- [禁止] 通过 `import` 导入的同名函数与当前文件内声明的顶层函数或同一文件内其他导入来源共同组成新的重载集合。
- [禁止] 多行 Lambda 使用 `->`。
- [禁止] 通过未标注类型的绑定、函数返回类型推导或其他无 callable-form `spec` 目标类型的表达式上下文为 Lambda 推导匿名函数类型。
- [禁止] 在不存在外层 `self` 绑定的上下文中直接引用 `self`。
- [建议] 保存重载函数值或重载方法值时,优先显式标注目标可调用形状以消除歧义。

## 6 编译期

- 编译器必须检查所有函数参数与 Lambda 参数是否显式标注类型。
- 编译器必须把 `mixable` 保存为成员级声明事实，并在同包 AST 与 `.ft` 恢复声明中
  提供一致查询结果，不得要求后续阶段重新解析注解文本；字段解释引用类型规范。
- 编译器必须检查 `@mixable` 的适用位置、参数个数、首参数变长标记、首参数
  object-form `spec` 形态，以及声明类型和展开目标按普通手写 `spec` 使用规则可见的名义
  满足关系。
- 编译器必须为展开目标生成保留完整签名和 `mixable` 事实的静态 wrapper，并对类型
  最终保留的每个 `@mixable` 静态方法无条件派生实例 wrapper。
- 编译器在加入来源静态 wrapper 候选前，必须检查目标显式成员的完整静态冲突；目标
  显式成员也是 `@mixable` 静态方法时，还必须按普通实例成员声明规则检查双方删除首
  参数后的实例投影冲突。该预检不得用于不同来源之间，也不得改变普通重载决议。
- 编译器必须让生成 wrapper 进入普通成员表，复用现有泛型、可见性、重载、冲突、
  `spec` 满足、方法值、导出和代码生成检查。
- 编译器必须为生成 wrapper 保存目标成员展开声明与来源静态方法声明的位置映射。
- 编译器必须使用同一直接 mix 授权查询处理 seal 来源候选、生成 wrapper 的来源调用、
  目标显式成员中的完整限定来源调用、重载可访问性过滤及 LSP 成员视图；不得根据声明
  来自同包 AST 还是 imported provider 选择不同语义路径。
- 编译器必须先排除无直接 mix 授权的 seal 静态候选，再进行普通重载解析；无授权
  候选不得遮蔽同名的可访问 open 候选。
- 编译器必须为 package-public `.ft` 恢复的 `seal + static + mixable` 方法提供稳定、
  可链接的跨包静态调用入口，同时保持其 Feng 可见性为 seal。
- 编译器必须检查参数 `let`/`var` 的使用是否合法,并按绑定规则验证参数赋值路径。
- 编译器必须检查构造函数与终结器函数是否满足 [Feng 语言类型规范](./feng-type.md) 中的声明约束。
- 编译器必须检查泛型 `extern func` 的每个参数位与返回位是否都可抹除为唯一且固定的外部调用 surface; 若某一位置会随具体类型实参改变外部 surface,或需要额外的类型描述符、wrapper 或按实例分化的导入签名,必须报错并阻止通过。
- 编译器必须检查泛型 `extern func` 调用点的类型实参是否完整且可确定: 显式提供时必须与类型参数个数一致; 省略时必须能由实参类型唯一推导出全部类型参数,否则必须报错并阻止通过。
- 编译器必须检查顶层函数与成员方法的重载集合是否存在返回值冲突或重叠匹配冲突；重叠检查必须纳入 object-form `spec` 上下文向上 coercion,包括具体 `type` 与其满足的 `spec`、子 `spec` 与其父 `spec`；构造函数相关检查见 [Feng 语言类型规范](./feng-type.md)。
- 编译器必须检查 `import` 导入的同名函数不会与当前文件内本地声明或同一文件内其他导入来源形成新的重载集合; 若形成重名冲突,必须报错。
- 编译器必须检查入口函数是否唯一且签名严格为 `main(args: string[])`,返回类型为 `void`。
- 编译器必须检查同一包中是否出现多个顶层 `main` 声明,超出一个则报错。
- 编译器必须检查 Lambda 的语法形式是否一致: 单表达式 Lambda 使用 `->`,多行 Lambda 使用块体且不写 `->`; 检查块体 Lambda 的 `return` 时必须使用该 Lambda 自身的 callable 返回上下文，不得套用外层 callable 的返回限制。
- 编译器必须检查 Lambda 值是否位于显式 callable-form `spec` 目标类型上下文中; 目标为泛型 callable-form `spec` 实例时,必须先实例化其参数与返回类型,并使用实例化后的签名检查 Lambda 或 `func`; 对显式 callable-form `spec` 返回类型下的 `return` Lambda,必须检查 Lambda 参数与返回结果是否匹配该返回类型; 对无显式 callable-form `spec` 返回类型的 `return` Lambda,必须报错并阻止通过。
- 编译器必须检查 `self` 的引用是否合法; Lambda 若引用 `self`,必须来自可捕获的外层实例成员方法、构造函数或终结器作用域。
- 编译器必须检查重载函数值或重载方法值在当前上下文下是否可以唯一确定目标可调用形状。
- 编译器解析 object-form `spec` 实例方法值时，必须复用对应实例方法直接调用的成员
  闭包、原声明 requirement、访问权限与签名替换结果，并把唯一选中的 requirement 和
  接收者 witness 视角稳定传递给代码生成；代码生成不得按名称重新选择成员。
- 编译器解析 object-form `spec` 约束下的泛型值实例方法值时，还必须保留 receiver 的
  完整 `T` 类型事实；共享泛型体与最终闭合代码必须消费同一已解析 requirement，不得
  依赖运行时成员搜索或把 receiver 改写为 spec 值。
- 编译器解析具体静态方法值时，必须稳定保留已经选中的 owner、fit、方法声明、owner
  实例类型、显式方法类型实参和目标 callable-form `spec`；本地与 imported 来源、普通
  代码与共享泛型体必须消费同一编译期来源身份，不得在代码生成或运行时按名称重选。
- 编译器解析受 object-form `spec` 约束的类型参数静态方法值时，必须稳定保留原声明
  requirement、caller 视角的 `T` 和目标 callable-form `spec`；最终闭合点必须使用该
  requirement 在闭合 `T` descriptor 中已有的 witness 槽，不得由名称重新选择成员。
- 编译器必须在语义分析阶段对以上违规报错并阻止通过。

## 7 运行时

- 函数调用在编译期完成重载决议后,运行时直接执行已确定的目标实现,不会按返回值再次分派。
- `@mixable` 静态与实例 wrapper 在运行时只执行编译期已确定的普通静态调用，不执行
  动态 receiver 重绑定，不保存来源实例或方法环境。
- 变长参数 wrapper 通过 `...args` 直接传递同一个预打包数组，不引入额外分配、复制、
  遍历或打包。
- 入口函数启动时会接收命令行参数数组 `args`。
- Lambda 捕获外部变量后形成闭包,闭包环境由运行时自动管理; 捕获为引用捕获——被捕获的 `var` 绑定与外层作用域共享存储,多个闭包捕获同一 `var` 时互相可见对方的赋值操作。
- Lambda 捕获外层 `self` 时，按接收者类型进入闭包环境：值类型复制当前值，Lambda 中的 `self` 引用闭包保存的值存储；引用类型复制引用，Lambda 中的 `self` 引用该引用所指向的同一实例。Lambda 自身不会生成新的 `self` 绑定。
- 方法值在形成时即绑定当前接收者：值类型按值捕获，引用类型复制引用；后续调用不会重新选择接收者，也不会因调用方法再次复制接收者。
- object-form `spec` 实例方法值在形成后直接通过已保存的 subject、witness 视角和
  requirement 槽调用，不执行运行时成员搜索、重载选择或接收者重绑定。
- object-form `spec` 约束下的泛型 receiver 在方法值形成时按闭合 `T` 复制或保留；形成
  结果只拥有一个 callable closure，不增加 spec box、第二个 receiver 分配或每次调用
  的动态候选查找。
- 完全闭合且无捕获的具体静态方法值使用编译期固定的静态 callable 表示；形成时不分配
  动态 closure，不执行成员搜索或运行时目标选择，调用时直接进入形成点选定的方法实现。
- 受 object-form `spec` 约束的类型参数静态方法值在闭合 `T` 时使用编译期固定的 immortal
  callable singleton；共享泛型体形成值时只读取既有 reified callable dependency 槽，
  不分配 closure、不查找成员，调用时直接进入已绑定的 witness 槽。
- 顶层函数、成员方法与闭包在运行时都表现为可调用值,但其可见性、是否重载以及是否绑定 `self` 由编译期规则先行确定。

## 8 关联

- [Feng 语言核心规范](./feng-language.md): 函数系统总览与跨章节总约束。
- [Feng 语言 `spec` 规范](./feng-spec.md): 可调用形状与契约规则。
- [Feng 语言变量绑定与作用域规范](./feng-binding.md): 参数 `let`/`var` 默认规则与赋值约束。
- [Feng 语言类型规范](./feng-type.md): `type`、构造函数、终结器函数与成员规则。
- [Feng 语言类型规范](./feng-type.md): `self`、方法值与对象行为细节。
- [Feng 语言对象生命周期规范](./feng-lifetime.md): 闭包环境的托管语义。
- [Feng 语言变长参数规范](./feng-function-variadic.md): 变长参数（`T...`）的语法、语义与编译期约束。
