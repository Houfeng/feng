# Feng 语言 `spec` 规范

本文档说明 Feng 中 `spec` 的职责、语法、语义与实现约束。`spec` 是 object-form、callable-form、union-form 与 intersection-form 的统一声明入口; 具体类型如何显式满足 object-form `spec` 契约,见 [feng-fit.md](./feng-fit.md); union-form 的成员选择、收窄与布局细则见 [feng-union-type.md](./feng-union-type.md)。

## 1 职责

- `spec` 用于声明 object-form 契约形状、callable-form 可调用形状、union-form 候选成员集合或 intersection-form 组合约束,不提供实现体。
- `spec` 可作为参数类型、返回类型、成员类型和其他类型位置中的引用目标。
- `type` 负责具体定义; object-form `spec` 负责契约目标; callable-form `spec` 负责可调用签名目标; union-form `spec` 负责值进入时的 active member 选择与后续收窄边界; intersection-form `spec` 负责组合多个 object-form 契约。

## 2 术语

- `spec`: 统一声明入口,可形成 object-form、callable-form、union-form 或 intersection-form。
- object-form `spec`: 对象形契约声明,定义字段与行为签名边界。
- callable-form `spec`: 可调用形状声明,定义参数列表与返回类型边界。
- union-form `spec`: 候选类型集合声明,表示一个值在进入该 `spec` 后处于直接成员集合中的其一。
- intersection-form `spec`: object-form 契约组合声明,表示值必须名义满足全部直接成员约束。
- active member: union-form 值当前实际持有的直接成员,由进入站点确定,由 union-form 的 `tag` 表达。
- `type`: 具体类型声明,可通过定义头或 `fit` 显式进入一个或多个 object-form `spec`。
- 契约满足: 指具体 `type` 满足目标 object-form `spec` 的全部字段与方法要求。
- 公开 requirement: object-form `spec` 中省略成员可见性修饰的字段或方法要求。
- `spec seal` requirement: object-form `spec` 中使用 `seal` 修饰、仅允许实现域
  通过 spec 视角访问的字段或方法要求。
- 方法签名: 方法名、方法泛参数量、参数个数、参数类型、参数顺序、变长参数
  形态与返回值类型的组合。方法泛参名称不属于签名身份。
- 默认 witness: `spec` 默认初始化时由语言规则提供的默认实例语义。

## 3 语法

正确语法一,对象形状与声明头满足:

```feng
spec Named {
  let name: string;
  func display(): string;
}

spec Identified {
  func id(): int;
}

type User: Named, Identified {
  let name: string;

  func display(): string {
    return self.name;
  }

  func id(): int {
    return 1;
  }
}
```

正确语法二,object-form `spec` 的 `seal` 成员:

```feng
spec Widget {
  func show(): void;
  seal func draw(): void;
  seal static let phase: int;
}
```

正确语法三,可调用形状:

```feng
spec Click(): void;
spec Mapper(x: int): int;
```

正确语法四,union-form:

```feng
spec Choice: int | string | User;
```

正确语法五,intersection-form:

```feng
spec ReadWrite: Readable & Writable;
spec Comparable<T>: Eq<T> & Ord<T>;
```

正确语法六,object-form spec 中声明静态成员:

```feng
// 静态字段与静态方法签名
spec Configurable {
  static let defaults: string;
  static var current: int;
}

// 泛型 spec 中声明静态成员,返回值与参数可使用 spec 类型参数
spec Factory<T> {
  static func make(): T;
  static let tag: string;
}

spec Registry<T> {
  static func all(): T[];
  static func byId(id: int): T;
  static let empty: T;
}

// 静态方法名可以与 spec 名相同,视为普通静态方法
spec Resource {
  static func Resource(): Resource;
}
```

`spec` 静态成员是对 `type` 静态能力的契约描述。当返回值或参数需要表达"实现类型自身"时,使用 spec 类型参数 `T`,由满足方在 `type Widget: Factory<Widget>` 中绑定。

错语法一,object-form `spec` 成员显式使用 `open`:

```feng
spec Bad {
  open func run(): void;
}
```

object-form `spec` 成员省略修饰时已经是公开 requirement；允许使用的唯一
成员可见性修饰符是 `seal`。

错语法二,`spec` 行为签名或可调用形状参数使用 `let` / `var` 修饰:

```feng
// 对象形状中的行为签名
spec Bad {
  func process(var x: int): void;
}

// 可调用形状
spec BadMapper(let x: int): int;
```

错语法三,`spec` 静态字段带初始值或静态方法带函数体:

```feng
spec Bad {
  static let zero: int = 0;        // spec 字段不能有初始值
  static func make(): Bad { ... }  // spec 方法签名不能有函数体
}
```

错语法四,循环声明满足:

```feng
spec A: B {}
spec B: A {}
```

错语法五,`type` 声明头满足 callable-form `spec`:

```feng
spec Click(): void;

type Button: Click {}
```

错语法六,`type` 声明头满足 union-form `spec`:

```feng
spec Choice: int | string;

type Box: Choice {}
```

错语法七,union-form `spec` 使用块体:

```feng
spec Choice: int | string {}
```

错语法八,intersection-form `spec` 使用块体:

```feng
spec ReadWrite: Readable & Writable {}
```

错语法九,`type` 声明头满足 intersection-form `spec`:

```feng
spec ReadWrite: Readable & Writable;

type Stream: ReadWrite {}
```

## 4 语义

- object-form `spec` 只约束可见形状（字段名、绑定方式 `let`/`var`、字段类型、行为签名与返回类型）,不约束具体内存布局、对象物理结构或 ABI 值布局。
- object-form `spec` 视角下的实例字段保持其声明类型的既有值模型。字段类型为 tuple、
  `@value type` 等复合值类型时,直接以该字段作为实例方法接收者必须作用于当前 spec
  subject 的实际字段存储,其行为与通过具体 `type` 视角访问同一字段一致;不得先复制字段、
  调用方法后再写回。
- 复合值实例字段进入绑定、参数、返回值或其他值存储边界时仍按值复制。例如
  `let copied = subject.valueField` 必须产生独立副本,后续修改原字段不得影响 `copied`,
  修改 `copied` 也不得影响原字段。若 subject 是值类型进入 spec 视角后形成的 box,
  “实际字段存储”指 box 内已经复制的 subject 字段,不反向修改装箱前的原值。
- 上述实际存储借用只属于编译器内部的方法接收者发码规则,不得在 Feng 层暴露字段地址,
  也不得改变普通引用类型字段的读取、赋值或方法调用行为。
- 由于对象形状 `spec` 不约束物理布局,运行时可采用分发表、witness 表或其他等价机制来满足契约,因此对象形状 `spec` 不构成 ABI 稳定类型,不能进入 C ABI 边界。
- 目前成员顺序的调整,不是兼容变更。未来增加基于编译期计算出稳定的成员 KEY 进行编译期成员重排的方式优化此问题。
- 可调用形状 `spec` 仅描述函数签名形状,不引入数据布局,因此可在标记 `@abi` 后作为 ABI 函数签名类型使用; 对应的原生函数指针类型写作 `Foo*`,详见 [Feng 语言 ABI 互操作规范](./feng-interop.md)。
- object-form `spec` 的字段与方法默认是公开 requirement；允许显式使用
  `seal` 将其声明为 `spec seal` requirement，不允许显式使用 `open`。
- 公开 requirement 与 `spec seal` requirement 均属于完整契约，均参与
  满足检查、父 spec 闭包和 witness 构造；`seal` 只把该 requirement 从
  普通 spec 访问面中排除。
- requirement 与实现成员的可见性兼容规则如下；省略修饰的 `type` 成员按
  其既有公开语义处理：

| object-form `spec` requirement | 公开或无修饰 `type` 成员 | `type seal` 成员 |
| --- | --- | --- |
| 公开或无修饰 | 允许满足 | 拒绝满足 |
| `seal` | 允许满足 | 允许满足 |

- 上述兼容规则统一适用于字段、方法、实例成员、静态成员、`type` 直接提供
  的实现和 `fit` 提供的方法实现。公开 requirement 已找到结构匹配但仅有
  `seal` 实现成员时，必须按实现成员可见性不兼容诊断，不得建立 witness。
- `type seal` 成员只有在满足关系由 `type` 声明头建立，或由与目标 `type`
  属于同一包的 `fit` 建立时，才可以按上表承担 `spec seal` requirement。
  跨包 `fit T: S` 不得把目标 `T` 的 `seal` 字段、方法或静态成员选为实现；
  它仍可使用 `T` 的公开成员，或使用该 `fit` 自己显式提供的方法实现。
  package-public `.ft` 为恢复类型布局而保留的私有字段事实不得扩大该权限。
- `spec S` 的 `seal` 成员只允许在以下实现域中通过能够暴露该成员的 spec
  静态视角访问：访问点位于满足 `S` 的 `type T` 自身成员方法、静态方法，
  或目标为 `T` 的 `fit` 扩展方法中。普通函数和不满足 `S` 的 type/fit
  上下文不得访问。
- 继承得到的 `spec seal` 成员保留原声明可见性，访问域按成员原声明 spec
  判断。实现子 spec 的 type 按现有名义闭包同时满足父 spec。
- `spec seal` 的访问权限只作用于 spec 视角和既有 witness 分派，不改变
  承担 requirement 的具体 type 成员可见性，也不允许通过具体 type 视角
  访问其他 type 或 fit 目标的 `seal` 实现成员。
- object-form `spec` 支持声明静态成员（`static let`、`static var`、`static func`）,作为 `type` 静态能力的契约描述;静态成员与实例成员采用一致的成员声明顺序。
- 对象形状中的行为签名使用 `func` 关键字,在 `spec` 中可不写函数体。
- object-form `spec` 是契约声明,成员面仅包含字段声明与方法签名; spec 一律不允许声明终结器（`~` 前缀的方法）。
- `spec` 中任何位置的参数均不可使用 `let` 或 `var` 修饰符,包括对象形状中的行为签名参数与可调用形状的参数; 参数可变性属于实现侧内部约束,不属于 `spec` 契约形状的一部分。
- `spec` 中的成员类型规则与 `type` 的成员类型引用规则一致: 成员类型必须引用已声明的具名类型,不能在成员类型位置内联匿名类型定义。
- 可调用形状使用 `spec Name(args): ReturnType;` 形式定义。
- union-form 使用 `spec Name: TypeRef ('|' TypeRef)+;` 形式定义,以分号结束,不允许 `{}` 块体; `|` 表示 OR 关系,不同于 object-form 父 `spec` 列表中的逗号 AND 关系。
- union-form member 可引用基础类型、用户定义类型与其他 `spec`; `void` 不允许作为 union-form member。
- union-form member 保持声明时的层次结构，不递归展开嵌套 union-form；成员去重作用在直接成员层面，并保持声明顺序。
- union-form 默认零值取直接成员列表中第一个 member 的默认零值。
- union-form 值在进入 union-form 的赋值、初始化、传参与返回等站点确定 active member；编译器在编译期通过多级链路查找，确定从源类型到目标 union-form 的完整路径（精确 member 优先，嵌套 union-form 间接匹配次之）；若存在多条可达路径则诊断为歧义；运行时仅执行路径已确定的 tag 设置与数据拷贝。
- union-form 未收窄前不允许直接做成员访问、方法调用或 `==` / `!=` 比较; 收窄通过 `match 目标值 { ... }` 的 union member 类型匹配完成,其详细规则见 [feng-union-type.md](./feng-union-type.md)。
- intersection-form 使用 `spec Name: SpecRef ('&' SpecRef)+;` 形式定义,以分号结束,不允许 `{}` 块体或自有成员; `&` 表示值必须同时满足全部成员约束。
- intersection-form 的直接 member 必须是 object-form 或 intersection-form `spec`; 多层 intersection-form 在编译期展平并去重,其成员方法集包含各 object-form 成员及其父 `spec` 闭包的方法集。
- 具体类型满足 intersection-form,当且仅当其名义满足该 intersection-form 展平后的全部 object-form 成员; 具体 `type` 不得在声明头或 `fit` 中直接列出 intersection-form `spec`。
- intersection-form 允许作为具名类型和泛型约束使用,但不支持内联 intersection、`match`/收窄或作为 union-form member。
- intersection-form 的同名同参数同返回类型方法去重; 同名同参数但返回类型不同构成冲突; 参数列表不同的方法保留为重载。
- 具体 `type` 可在声明头上直接写出其满足的一个或多个 object-form `spec`; 同一关系也可通过可见的 `fit A: SpecB` 或 `fit A: SpecB, SpecC` 显式建立。
- callable-form `spec` 只描述可调用签名形状,不能作为 `type A: SpecB` 或 `fit A: SpecB` 这类声明满足关系的目标。
- callable-form `spec` 的默认零值必须是可安全调用的零捕获空操作 callable；其实现不得捕获、绑定或读取任何变量，调用时也不得访问空指针。返回 `void` 时不执行其他行为，返回非 `void` 时返回声明返回类型的默认零值。
- callable-form `spec` 的隐式匹配采用两段规则: 未绑定到 `spec` 的非泛型顶层函数、非泛型方法值、已通过显式泛型 target 完成闭合的函数或方法以及 lambda，进入 callable-form `spec` 位置时按“参数个数 + 参数类型 + 参数顺序 + 返回值类型完全一致”做结构匹配；来源函数或方法自身声明泛参时，必须先显式提供完整类型实参，目标 `spec` 不参与推导来源泛参。一旦值的静态类型已经是某个 callable-form `spec`,后续赋值、参数传递与返回匹配只允许同一 callable-form `spec` 声明。
- 不同 callable-form `spec` 即使签名完全一致也不得隐式互相匹配; 仅当两个 callable-form `spec` 在实例化后的参数类型与返回类型完全一致时,才允许显式转换。
- callable-form `spec` 的显式转换目标可以直接承接尚未绑定到 callable-form `spec` 的顶层函数或实例方法引用，并按目标完成普通结构匹配和 callable value 形成。泛型函数或方法来源必须先显式提供完整类型实参；转换目标不得反向推导来源泛参。
- callable-form `spec` 的显式转换资格必须在编译期确定; 运行时不得重新比较签名、搜索候选或决定转换是否成立。
- callable-form `spec` 的显式转换一旦合法,编译器必须直接按目标 callable-form `spec` 视角发码; 对实例化后签名完全一致的 callable-form `spec`,该发码只允许切换静态视角,不得构造新的 wrapper/closure、不得插入额外转发层,且转换后通过该值发起的每次调用开销必须小于等于转换前; 运行时不得再做动态适配或回退。
- union-form `spec` 只描述值进入时的 member 选择与收窄边界,不能作为 `type A: SpecB` 或 `fit A: SpecB` 这类声明满足关系的目标; union-form 的专门规则见 [feng-union-type.md](./feng-union-type.md)。
- intersection-form `spec` 只描述多个 object-form 契约的组合约束,不能作为 `type A: SpecB` 或 `fit A: SpecB` 这类声明满足关系的目标。
- 对象形状 `spec` 支持沿已显式声明的名义契约关系建立父视角: 具体 `type` 可进入当前可见契约闭包中已证明满足的 object-form `spec` 位置; 子 object-form `spec` 值也可进入其直接或传递父 `spec` 位置。
- 赋值、初始化、传参、返回、字段写入、数组元素写入及已由合法无重叠重载集合唯一确定目标参数类型的调用位置允许上述上下文向上 coercion。该 coercion 是由开发者已经声明的 `type T: S`、`fit T: S` 或 `spec S1: S2` 名义关系建立的契约视角投影,不构成无名义关系类型之间的一般隐式转换。
- 上下文向上 coercion 参与现有重载重叠检查: 若同一实参类型可精确匹配一个候选并向上 coercion 到另一个候选,或可向上 coercion 到多个候选,该重载集合必须在声明阶段视为冲突; 不以“精确优先”或“最具体优先”消解重叠候选。
- 具体 `type` 与 object-form `spec` 值也可通过显式 cast 建立上述同一父视角; 显式形式不扩大可达的契约关系集合。
- 对象形状 `spec` 的上下文向上 coercion 与显式 cast 资格必须在编译期确定; 运行时不得重新搜索满足关系,也不得依据对象真实具体类型临时决定转换是否成立。
- 对象形状 `spec` 的上下文向上 coercion 或显式 cast 一旦合法,编译器必须直接按目标 `spec` 视角发码; 运行时不得再做候选 `spec` 搜索、试探转换或回退。

### 4.1 spec 静态成员

- object-form `spec` 中允许声明 `static let`、`static var` 与 `static func` 成员,作为 `type` 静态能力的契约描述; spec 不提供任何实现,静态字段声明不带初始值,静态方法签名不带函数体。
- spec 静态字段声明必须以 `;` 结束,spec 静态方法签名必须以 `;` 结束;静态方法参数不可使用 `let` 或 `var` 修饰符,且必须显式声明返回类型。
- spec 静态方法与实例方法一样，当前均不得自己声明方法级泛参；泛型 spec owner
  的类型参数仍可用于静态方法签名。Parser 继续识别方法泛参语法，Semantic 必须
  在 object-form spec 成员签名检查处拒绝。spec 静态方法支持重载，规则与 `type`
  中静态方法一致；spec 静态成员允许 `seal`,不允许显式
  `open`,并使用与实例成员相同的满足兼容和访问域规则。
- spec 方法名可以与 spec 名相同（包括静态方法和实例方法）,视为普通方法; spec 一律不允许 `~` 前缀的方法（终结器只允许用于 `type`）。
- spec 静态字段的满足来源只能是 `type` 自身（`fit` 不得声明 `static let` / `static var`）; spec 静态方法的满足来源可以是 `type` 自身或可见 `fit` 中的静态方法。
- spec 静态字段匹配采用"名称 + 绑定方式（`let` / `var`） + 类型完全一致"规则;
  spec 静态方法复用实例方法的完整签名匹配规则，采用“名称 + 参数个数 + 参数
  类型 + 参数顺序 + 变长参数形态 + 返回值类型完全一致”规则。
- 泛型 spec 中的类型参数在满足检查时按 `type Widget: Factory<Widget>` 中的实参替换后精确匹配,复用与 spec 实例方法相同的机制。
- 通过具体类型名访问静态成员（如 `Widget.make()`、`Widget.tag`）为直接调用或直接访问,运行时不引入额外开销。
- 通过泛型约束中的类型参数访问静态成员（如 `T.make()`、`T.field`,其中 `T: SomeSpec`）通过编译期 witness 表完成静态分派,不开销与现有泛型实例方法分派一致。
- 不允许通过实例访问静态成员（与 `type` 规则一致）; 不允许通过 spec 值访问静态成员（spec 值是实例级概念）。

## 5 规则

分为「必须、禁止、建议」。

- [必须] 在 `spec Foo: Bar, Baz {}` 中,冒号右侧必须是一个或多个 `spec`,并使用逗号分隔。
- [必须] union-form 必须写作 `spec Foo: A | B | C;`,右侧至少包含两个 member,并使用 `|` 分隔。
- [禁止] union-form `spec` 使用 `{}` 块体。
- [禁止] `void` 作为 union-form member。
- [必须] intersection-form 必须写作 `spec Foo: A & B & C;`,右侧至少包含两个 member,并使用 `&` 分隔。
- [禁止] intersection-form `spec` 使用 `{}` 块体、声明自有成员或出现在 union-form member 中。
- [必须] intersection-form member 必须是 object-form 或 intersection-form `spec`; 多层 intersection-form 必须在编译期展平并去重。
- [禁止] object-form `spec` 的父 `spec` 列表中出现 callable-form、union-form 或 intersection-form `spec`；object-form `spec` 的父级只能是 object-form `spec`。
- [必须] 在 `type Foo: Bar, Baz {}` 或契约适配 `fit Foo: Bar, Baz` 中,冒号右侧每一项都必须是 object-form `spec`。
- [必须] 判断 `type` 是否满足 `spec` 时,字段匹配采用“名称 + 绑定方式（`let` 或 `var`，即字段是否可变） + 类型完全一致”规则。
- [禁止] object-form `spec` 的实例方法或静态方法自己声明方法级泛参。Parser
  必须继续接受并把泛参完整记录到 AST；Semantic 必须在编译该 spec 的成员方法
  签名时以前置诊断拒绝，并停止该声明进入后续解析、满足检查或 Codegen。该限制
  不影响泛型 spec owner、type/fit 泛型方法、泛型函数、泛型约束或 callable-form
  spec 的类型级泛参。
- [必须] 判断 `type` 是否满足 object-form `spec` 时，方法匹配采用
  “名称 + 参数个数 + 参数类型 + 参数顺序 + 变长参数形态 + 返回值类型完全
  一致”规则；泛型 spec owner 的实参必须先替换到 requirement 签名，再与
  `type` 自有实现或 `fit` 实现比较。实现方法自身声明方法泛参时，与非泛型
  requirement 的泛参数量不同，不得满足该 requirement。
- [必须] object-form `spec` 的公开 requirement 只能由公开或无修饰的实现
  成员满足；`spec seal` requirement 可以由公开、无修饰或 `seal` 实现
  成员满足。字段、方法、实例、静态、type 与 fit 来源使用同一规则。
- [必须] 当 `fit T: S` 与目标 `T` 不属于同一包时，满足检查和 witness
  选择必须排除 `T` 自身的全部 `seal` 成员；同包 `fit` 保持允许，当前
  `fit` 自己声明的方法仍按 requirement/实现成员可见性兼容规则参与满足。
- [必须] 满足 object-form `spec` 的 type 成员方法、静态方法及其 fit 扩展
  方法可以通过 spec 视角访问该 spec 的 `seal` 成员；普通函数和不满足
  该 spec 的实现上下文不得访问。
- [禁止] 使用 `spec seal` 访问权限扩大任何具体 type 成员的可见性，或
  通过具体 type 视角访问其 `seal` 实现成员。
- [必须] 当 object-form `spec` 继承父 `spec` 后与父 `spec` 出现同名且签名完全一致的方法时,成员解析与调用重载集合必须按“子 `spec` 优先”去重处理,不得把该父方法当作额外重载候选。
- [必须] 当同一个 `type` 同时满足多个 `spec` 时,若出现“方法名相同,且参数个数、参数类型和参数顺序一致,但返回值类型不一致”的方法签名,必须视为冲突。
- [必须] 若某个列出的 `spec` 还要求满足其他 `spec`,则该 `type` 也必须同时满足这些额外 `spec` 的全部要求。
- [禁止] `spec` 循环声明满足; `spec` 之间形成直接或间接循环满足关系时禁止通过。
- [禁止] 同一声明头中的 `spec` 列表重复列出同一个 `spec`。
- [禁止] 在 `type` 声明头或契约适配 `fit` 中把 callable-form、union-form 或 intersection-form `spec` 当作满足目标。
- [禁止] `spec` 中任何参数位置使用 `let` 或 `var` 修饰符,包括对象形状中的行为签名参数与可调用形状的参数。
- [禁止] object-form `spec` 声明终结器（`~` 前缀的方法）; 该限制属于语义规则,由语义分析阶段诊断。
- [必须] object-form `spec` 中声明的 `static let` / `static var` / `static func` 必须不带初始值或函数体;静态字段声明必须以 `;` 结束,静态方法签名必须以 `;` 结束,静态方法必须显式声明返回类型。
- [必须] object-form `spec` 的实例字段、实例方法、静态字段和静态方法允许
  使用 `seal`，省略修饰时保持公开语义；显式 `open` 必须被拒绝。
- [必须] object-form `spec` 的实例字段、实例普通方法、静态字段和静态普通方法在
  显式 `seal` 时允许使用 `@friend`；授权继续通过 spec witness，且不得改变
  requirement 满足与具体实现成员可见性。完整规则统一见
  [Feng 语言可见性规范](./feng-visibility.md#103-friend-seal-成员)。
- [必须] object-form `spec` 的静态字段满足来源只能是 `type` 自身; spec 静态方法满足来源可以是 `type` 自身或可见 `fit` 中的静态方法。
- [禁止] 对象形状的 `spec` 不得标记 `@abi` 或任何调用方式注解; `@abi` 仅适用于 callable-form 的 `spec`。
- [必须] 未绑定到 callable-form `spec` 的非泛型顶层函数、非泛型方法值、已显式闭合的泛型函数或方法以及 lambda 在进入 callable-form `spec` 位置时,必须按“参数个数 + 参数类型 + 参数顺序 + 返回值类型完全一致”进行结构匹配。
- [必须] 泛型函数或方法作为值时，必须显式提供完整类型实参并先完成实参数量、约束和签名替换检查；callable-form `spec` 目标不得隐式推导来源泛参。
- [必须] 静态类型已经是 callable-form `spec` 的值在进入另一 callable-form `spec` 位置时,只允许同一 callable-form `spec` 声明隐式匹配。
- [必须] 不同 callable-form `spec` 之间的显式转换仅在实例化后的签名完全一致时允许,且资格必须在编译期确定。
- [必须] callable-form `spec` 的显式转换操作数可以是尚未绑定到 spec 的顶层函数或实例方法引用；泛型来源必须写出完整显式类型实参，且闭合后的签名必须与转换目标完全一致。
- [必须] 当 callable-form `spec` 的显式转换因实例化后签名完全一致而成立时,编译器必须将其 lower 为不增加每次调用开销的目标视角切换; 不得为此分配新的 wrapper/closure,也不得增加额外 invoke 转发层。
- [禁止] 不同 callable-form `spec` 仅因签名结构相同而发生隐式匹配。
- [必须] 对象形状 `spec` 的上下文向上 coercion 只允许两类名义视角投影: 具体 `type` 到其当前可见契约闭包中已满足的 object-form `spec`,以及子 object-form `spec` 到其直接或传递父 object-form `spec`。
- [必须] 上述上下文向上 coercion 适用于赋值、初始化、传参、返回、字段写入、数组元素写入及已由合法无重叠重载集合唯一确定目标参数类型的调用位置；它不得扩展为无名义契约关系类型之间的一般隐式转换。
- [必须] object-form `spec` 的上下文向上 coercion 必须参与普通重载重叠检查；若同一实参类型可匹配同一重载集合中的多个候选,必须在声明阶段视为签名冲突,不得通过精确匹配优先级、父级距离或调用点显式 cast 保留该重载集合。
- [必须] 对象形状 `spec` 的显式 cast 允许建立与上下文向上 coercion 相同的两类父视角,但不得扩大可达的契约关系集合。
- [必须] 对象形状 `spec` 的上下文向上 coercion 与显式 cast 资格必须仅依据当前可见契约关系在编译期确定。
- [必须] 对象形状 `spec` 的上下文向上 coercion 或显式 cast 一旦成立,编译器必须直接构造静态已知的目标 `spec` 视角; 运行时不得再做候选搜索、试探或回退。
- [禁止] 从父 object-form `spec` 到子 object-form `spec`、无关 object-form `spec` 之间、以及依赖运行时对象具体类型才可能成立的上下文 coercion 或显式 cast。
- [必须] union-form 进入站点必须在编译期决定 active member; 当多个 object-form `spec` member 可同时接纳同一源值且不存在精确 member 命中时,必须诊断为歧义,不得按声明顺序兜底。
- [禁止] 当前阶段直接把 union-form 视角值显式转换到共同 object-form `spec`,即使该 union-form 的全部 member 都满足该共同 `spec`。
- [必须] 具体类型满足 intersection-form,当且仅当其名义满足该 intersection-form 展平后的全部 object-form member。
- [必须] intersection-form 的成员方法集合并必须对完全相同的签名去重,保留参数列表不同的重载,并拒绝同名同参数但返回类型不同的冲突。
- [禁止] 内联 intersection、intersection-form `match`/收窄以及显式声明满足 intersection-form。
- [必须] 可调用形状的 `spec` 其参数类型与返回类型必须符合 [Feng 语言 ABI 互操作规范](./feng-interop.md) 中定义的 ABI 函数签名兼容规则，才能标记为 `@abi`。
- [建议] 直接写在 `type` 声明头上的满足关系用于表达“定义者主动承诺”; `fit` 优先用于无法修改原始 `type` 定义时的非侵入适配。

## 6 编译期

- 编译器必须检查 `spec` 声明头右侧是否仅包含 `spec`。
- 编译器必须区分 object-form、callable-form、union-form 与 intersection-form `spec`,并按各自语法形态解析。
- 编译器必须检查 union-form member 列表是否合法,拒绝少于两个 member、`void` member 与 `{}` 块体。
- 编译器必须保持 union-form 的声明时层次结构，不递归展开嵌套 union-form；在直接成员层面去重并保持声明顺序；在赋值等进入站点做编译期多级链路查找，确定从源类型到目标 union-form 的路径，并在存在多条可达路径时诊断为歧义。
- 编译器必须检查 intersection-form member 是否均为 object-form 或 intersection-form `spec`,在编译期展平并去重多层 intersection-form,并拒绝其使用块体、自有成员或出现在 union-form member 中。
- 编译器必须检查 object-form `spec` 的父 `spec` 列表中每一项是否均为 object-form `spec`，并拒绝 callable-form、union-form 与 intersection-form `spec` 出现在父 `spec` 列表中。
- 编译器必须检查 `type` 声明头与契约适配 `fit` 的右侧是否全部为 object-form `spec`,并拒绝 callable-form、union-form 与 intersection-form `spec`。
- 编译器必须检查 `type` 对目标 `spec` 的字段与方法是否满足精确匹配规则。
- 编译器必须在满足验证、快速满足查询和 witness 选择中使用同一份
  requirement/实现成员可见性兼容判断。
- 编译器必须在 spec 字段读取与写入、方法调用、方法值、静态约束成员访问
  和重载候选选择时检查 `spec seal` 访问域；不可访问候选不得参与重载。
- 编译器必须在 Parser 阶段接受 object-form `spec` 体内的 `static let` / `static var` / `static func` 声明,并在语义阶段以与 `type` 静态成员一致的规则对签名、可见性、`~` 前缀做检查。
- 编译器必须检查并拒绝 `spec` 循环声明满足关系。
- 编译器必须检查并拒绝“同名同参数顺序但返回值不一致”的多 `spec` 方法冲突。
- 编译器必须在 object-form `spec` 的父子闭包成员收集中对“同名且签名完全一致”的方法按“子 `spec` 优先”去重,避免把继承覆盖关系误判为多重重载歧义。
- 编译器必须检查并拒绝 `spec` 列表中的重复项。
- 编译器必须在语义分析阶段检查并拒绝 object-form `spec` 中的终结器声明（`~` 前缀的方法）。
- 编译器必须在语义分析阶段检查 `type` 是否为 object-form `spec` 中的静态成员提供了实现;静态字段必须由 `type` 自身提供,静态方法可由 `type` 自身或可见 `fit` 提供。
- 编译器必须检查并拒绝在对象形状 `spec` 上使用 `@abi` 或调用方式注解。
- 编译器必须区分“未绑定可调用值 → callable-form `spec`”的结构匹配与“callable-form `spec` → callable-form `spec`”的名义匹配。
- 编译器必须仅在两个 callable-form `spec` 的实例化后签名完全一致时接受显式转换,并在语义分析阶段拒绝其他 callable-form `spec` 转换。
- 编译器必须把实例化后签名完全一致的 callable-form `spec` 显式转换 lower 为零转发的目标视角重解释; 不得为该转换生成新的 wrapper/closure,也不得让转换后的每次调用比转换前多一层 invoke forwarding。
- 编译器必须在语义分析阶段根据当前可见契约关系判定对象形状 `spec` 的上下文 coercion 与显式 cast 是否属于允许的向上视角投影,并拒绝父到子、无关 `spec` 或依赖运行时对象具体类型的转换。
- 编译器必须在赋值、初始化、传参、返回、字段写入、数组元素写入及重载重叠检查中统一应用 object-form `spec` 上下文向上 coercion；若具体 `type` 与其满足的 `spec`、子 `spec` 与其父 `spec`，或其他契约关系使同一实参类型可匹配多个重载候选,必须在声明阶段诊断签名冲突。
- 编译器必须在 union-form 进入站点按精确直接 member 优先、嵌套 union-form 多级链路间接匹配次之的规则确定 active member 路径；多条可达路径构成歧义时必须报错，不得按声明顺序兜底。
- 编译器必须在 union-form `match 目标值 { ... }` 中只接受 union 直接成员类型标签与 `else`，拒绝字面量标签和区间标签；穷尽性检查只验证直接成员是否被覆盖。
- 编译器必须在 intersection-form 使用位置检查源类型是否名义满足展平后的全部 object-form member,并使用合并 witness 支持成员访问与泛型约束。
- 编译器必须合并 intersection-form 全部成员及其父 `spec` 闭包的方法集,对完全相同的签名去重,保留合法重载并诊断返回类型冲突。
- 编译器必须按 [Feng 语言 ABI 互操作规范](./feng-interop.md) 校验可调用形状的 `@abi spec` 的参数类型与返回类型是否满足 ABI 函数签名兼容规则。
- 编译器必须在语义分析阶段对以上违规报错并阻止通过。

## 7 运行时

- 无初始值的 `spec` 绑定会自动得到该 `spec` 的默认 witness。
- 默认 witness 由编译器自动生成,与对应 `spec` 属于同一模块,但对开发者不可见也不可显式引用; 每个 `spec` 都有对应默认 witness,且必须满足该 `spec` 声明的全部成员与签名要求。
- 字段成员按各自类型的默认零值初始化。
- 返回 `void` 的行为签名提供空实现; 返回非 `void` 的行为签名返回其返回类型默认零值; 若返回值是 `spec` 类型,则返回该 `spec` 对应的默认 witness。
- 通过具体类型名访问 spec 静态成员的运行时开销与访问 `type` 静态成员完全一致; 通过泛型类型参数访问 spec 静态成员时,采用与 spec 实例方法 witness 表一致的间接分派策略,不在运行时引入额外的候选搜索或回退。
- 每次对 `spec` 类型执行默认初始化时,都会创建该 `spec` 默认 witness 的新实例,不复用共享单例。
- object-form 与 intersection-form `spec` 值上的 `==` / `!=` 比较其 subject 引用身份，不执行深度比较；callable-form `spec` 值上的 `==` / `!=` 比较 callable/closure 引用身份，不比较捕获内容、绑定对象或调用签名。union-form `spec` 未收窄前仍不允许直接使用 `==` / `!=`。
- 代码以 `spec` 视角访问成员或发起调用时,运行时可采用分发表、内联缓存、静态去虚化或其他等价策略完成成员映射与分发。
- callable-form `spec` 的显式转换不引入运行时签名比较、候选搜索、wrapper/closure 分配或额外调用转发; 对实例化后签名完全一致的 callable-form `spec`,转换前后经该值发起的调用开销必须保持同级。
- 对象形状 `spec` 的上下文向上 coercion 与显式 cast 不引入运行时满足关系搜索、候选比较或回退; 运行时只执行编译期已选定的 `spec` 视角构造与成员分发。
- union-form 统一映射为既有 `aggregate-with-managed-slots` 顶层值模型,首版值布局为 `tag + _fwd + payload`; `tag` 表达 active member identity,`_fwd` 只表达当前 payload 生命周期路径,不得把 union-form 实现为新的 runtime top-level value kind。
- intersection-form 与 object-form `spec` 使用等价的对象视角与 witness 分发模型; 合并 witness 在编译期确定,成员访问开销不得大于 object-form `spec` 视角。
- 运行时实现不强制绑定单一机制; 只要求满足高性能、0 开销或极低开销、ABI 稳定。

## 8 关联

- [Feng 语言 `fit` 规范](./feng-fit.md): `fit` 适配、导出与作用域规则。
- [Feng 语言类型规范](./feng-type.md): 类型系统与成员类型引用规则。
- [Feng 联合类型规范](./feng-union-type.md): union-form 的成员选择、收窄、泛型约束与布局细则。
- [Feng 语言核心规范](./feng-language.md): 语言总规范与跨章节总约束。
