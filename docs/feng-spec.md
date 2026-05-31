# Feng 语言 `spec` 规范

本文档说明 Feng 中 `spec` 的职责、语法、语义与实现约束。`spec` 是 object-form、callable-form 与 union-form 的统一声明入口; 具体类型如何显式满足 object-form `spec` 契约,见 [feng-fit.md](./feng-fit.md); union-form 的成员选择、收窄与布局细则见 [feng-union-type.md](./feng-union-type.md)。

## 1 职责

- `spec` 用于声明 object-form 契约形状、callable-form 可调用形状或 union-form 候选成员集合,不提供实现体。
- `spec` 可作为参数类型、返回类型、成员类型和其他类型位置中的引用目标。
- `type` 负责具体定义; object-form `spec` 负责契约目标; callable-form `spec` 负责可调用签名目标; union-form `spec` 负责值进入时的 active member 选择与后续收窄边界。

## 2 术语

- `spec`: 统一声明入口,可形成 object-form、callable-form 或 union-form。
- object-form `spec`: 对象形契约声明,定义字段与行为签名边界。
- callable-form `spec`: 可调用形状声明,定义参数列表与返回类型边界。
- union-form `spec`: 候选类型集合声明,表示一个值在进入该 `spec` 后处于归一化 member 集合中的其一。
- active member: union-form 值当前实际持有的归一化 member,由进入站点确定,由 union-form 的 `tag` 表达。
- `type`: 具体类型声明,可通过定义头或 `fit` 显式进入一个或多个 object-form `spec`。
- 契约满足: 指具体 `type` 满足目标 object-form `spec` 的全部字段与方法要求。
- 方法签名: 方法名、参数个数、参数类型、参数顺序与返回值类型的组合。
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

正确语法二,可调用形状:

```feng
spec Click(): void;
spec Mapper(x: int): int;
```

正确语法三,union-form:

```feng
spec Choice: int | string | User;
```

错语法一,`spec` 中显式可见性修饰:

```feng
spec Bad {
  open func run(): void;
}
```

错语法二,`spec` 行为签名或可调用形状参数使用 `let` / `var` 修饰:

```feng
// 对象形状中的行为签名
spec Bad {
  func process(var x: int): void;
}

// 可调用形状
spec BadMapper(let x: int): int;
```

错语法三,`spec` 中声明静态成员:

```feng
spec Bad {
  static func make(): Bad;
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

## 4 语义

- object-form `spec` 只约束可见形状（字段名、绑定方式 `let`/`var`、字段类型、行为签名与返回类型）,不约束具体内存布局、对象物理结构或 ABI 值布局。
- 由于对象形状 `spec` 不约束物理布局,运行时可采用分发表、witness 表或其他等价机制来满足契约,因此对象形状 `spec` 不构成 ABI 稳定类型,不能进入 C ABI 边界。
- 目前成员顺序的调整,不是兼容变更。未来增加基于编译期计算出稳定的成员 KEY 进行编译期成员重排的方式优化此问题。
- 可调用形状 `spec` 仅描述函数签名形状,不引入数据布局,因此可在标记 `@abi` 后作为 ABI 函数签名类型使用; 对应的原生函数指针类型写作 `Foo*`,详见 [Feng 语言 ABI 互操作规范](./feng-interop.md)。
- `spec` 中的成员与行为签名默认公开,且不允许显式添加 `open` 或 `seal`。
- `spec` 中暂不支持声明静态成员; object-form `spec` 体内出现 `static` 时,编译器在 Parser 阶段拒绝。
- 对象形状中的行为签名使用 `func` 关键字,在 `spec` 中可不写函数体。
- object-form `spec` 是契约声明,成员面仅包含字段声明与方法签名; 不允许声明构造器或终结器。
- `spec` 中任何位置的参数均不可使用 `let` 或 `var` 修饰符,包括对象形状中的行为签名参数与可调用形状的参数; 参数可变性属于实现侧内部约束,不属于 `spec` 契约形状的一部分。
- `spec` 中的成员类型规则与 `type` 的成员类型引用规则一致: 成员类型必须引用已声明的具名类型,不能在成员类型位置内联匿名类型定义。
- 可调用形状使用 `spec Name(args): ReturnType;` 形式定义。
- union-form 使用 `spec Name: TypeRef ('|' TypeRef)+;` 形式定义,以分号结束,不允许 `{}` 块体; `|` 表示 OR 关系,不同于 object-form 父 `spec` 列表中的逗号 AND 关系。
- union-form member 可引用基础类型、用户定义类型与其他 `spec`; `void` 不允许作为 union-form member。
- union-form member 在语义层按声明顺序拍平嵌套 union-form、去重并形成归一化 member 集合; union-form 默认零值取归一化后的第一个 member 的默认零值。
- union-form 值在进入 union-form 的赋值、初始化、传参与返回等站点确定 active member; 若源静态类型与某个归一化 member 精确一致,必须优先按该 member 进入。
- union-form 未收窄前不允许直接做成员访问、方法调用或 `==` / `!=` 比较; 收窄通过 `if 目标值 { ... }` 的 union member 类型匹配完成,其详细规则见 [feng-union-type.md](./feng-union-type.md)。
- 具体 `type` 可在声明头上直接写出其满足的一个或多个 object-form `spec`; 同一关系也可通过可见的 `fit A: SpecB` 或 `fit A: SpecB, SpecC` 显式建立。
- callable-form `spec` 只描述可调用签名形状,不能作为 `type A: SpecB` 或 `fit A: SpecB` 这类声明满足关系的目标。
- callable-form `spec` 的隐式匹配采用两段规则: 未绑定到 `spec` 的顶层函数、方法值与 lambda 进入 callable-form `spec` 位置时继续按“参数个数 + 参数类型 + 参数顺序 + 返回值类型完全一致”做结构匹配; 一旦值的静态类型已经是某个 callable-form `spec`,后续赋值、参数传递与返回匹配只允许同一 callable-form `spec` 声明。
- 不同 callable-form `spec` 即使签名完全一致也不得隐式互相匹配; 仅当两个 callable-form `spec` 在实例化后的参数类型与返回类型完全一致时,才允许显式转换。
- callable-form `spec` 的显式转换资格必须在编译期确定; 运行时不得重新比较签名、搜索候选或决定转换是否成立。
- callable-form `spec` 的显式转换一旦合法,编译器必须直接按目标 callable-form `spec` 视角发码; 对实例化后签名完全一致的 callable-form `spec`,该发码只允许切换静态视角,不得构造新的 wrapper/closure、不得插入额外转发层,且转换后通过该值发起的每次调用开销必须小于等于转换前; 运行时不得再做动态适配或回退。
- union-form `spec` 只描述值进入时的 member 选择与收窄边界,不能作为 `type A: SpecB` 或 `fit A: SpecB` 这类声明满足关系的目标; union-form 的专门规则见 [feng-union-type.md](./feng-union-type.md)。
- 对象形状 `spec` 的显式转换只允许向上建立视角: 具体 `type` 可显式转换到当前可见契约闭包中已证明满足的 object-form `spec`; object-form `spec` 也可显式转换到其当前可见父 `spec` 视角。
- 对象形状 `spec` 的显式转换资格必须在编译期确定; 运行时不得重新搜索满足关系,也不得依据对象真实具体类型临时决定转换是否成立。
- 对象形状 `spec` 的显式转换一旦合法,编译器必须直接按目标 `spec` 视角发码; 运行时不得再做候选 `spec` 搜索、试探转换或回退。

## 5 规则

分为「必须、禁止、建议」。

- [必须] 在 `spec Foo: Bar, Baz {}` 中,冒号右侧必须是一个或多个 `spec`,并使用逗号分隔。
- [必须] union-form 必须写作 `spec Foo: A | B | C;`,右侧至少包含两个 member,并使用 `|` 分隔。
- [禁止] union-form `spec` 使用 `{}` 块体。
- [禁止] `void` 作为 union-form member。
- [禁止] object-form `spec` 的父 `spec` 列表中出现 callable-form `spec` 或 union-form `spec`；object-form `spec` 的父级只能是 object-form `spec`。
- [必须] 在 `type Foo: Bar, Baz {}` 或契约适配 `fit Foo: Bar, Baz` 中,冒号右侧每一项都必须是 object-form `spec`。
- [必须] 判断 `type` 是否满足 `spec` 时,字段匹配采用“名称 + 绑定方式（`let` 或 `var`，即字段是否可变） + 类型完全一致”规则。
- [必须] 判断 `type` 是否满足 `spec` 时,方法匹配采用“名称 + 参数个数 + 参数类型 + 参数顺序 + 返回值类型完全一致”规则。
- [必须] 当 object-form `spec` 继承父 `spec` 后与父 `spec` 出现同名且签名完全一致的方法时,成员解析与调用重载集合必须按“子 `spec` 优先”去重处理,不得把该父方法当作额外重载候选。
- [必须] 当同一个 `type` 同时满足多个 `spec` 时,若出现“方法名相同,且参数个数、参数类型和参数顺序一致,但返回值类型不一致”的方法签名,必须视为冲突。
- [必须] 若某个列出的 `spec` 还要求满足其他 `spec`,则该 `type` 也必须同时满足这些额外 `spec` 的全部要求。
- [禁止] `spec` 循环声明满足; `spec` 之间形成直接或间接循环满足关系时禁止通过。
- [禁止] 同一声明头中的 `spec` 列表重复列出同一个 `spec`。
- [禁止] 在 `type` 声明头或契约适配 `fit` 中把 callable-form `spec` 或 union-form `spec` 当作满足目标。
- [禁止] `spec` 中任何参数位置使用 `let` 或 `var` 修饰符,包括对象形状中的行为签名参数与可调用形状的参数。
- [禁止] `spec` 中声明 `static` 成员; 该限制由 Parser 阶段诊断。
- [禁止] object-form `spec` 声明构造器或终结器; 该限制属于语义规则,由语义分析阶段诊断。
- [禁止] 对象形状的 `spec` 不得标记 `@abi` 或任何调用方式注解; `@abi` 仅适用于 callable-form 的 `spec`。
- [必须] 未绑定到 callable-form `spec` 的顶层函数、方法值与 lambda 在进入 callable-form `spec` 位置时,必须按“参数个数 + 参数类型 + 参数顺序 + 返回值类型完全一致”进行结构匹配。
- [必须] 静态类型已经是 callable-form `spec` 的值在进入另一 callable-form `spec` 位置时,只允许同一 callable-form `spec` 声明隐式匹配。
- [必须] 不同 callable-form `spec` 之间的显式转换仅在实例化后的签名完全一致时允许,且资格必须在编译期确定。
- [必须] 当 callable-form `spec` 的显式转换因实例化后签名完全一致而成立时,编译器必须将其 lower 为不增加每次调用开销的目标视角切换; 不得为此分配新的 wrapper/closure,也不得增加额外 invoke 转发层。
- [禁止] 不同 callable-form `spec` 仅因签名结构相同而发生隐式匹配。
- [必须] 对象形状 `spec` 的显式转换只允许两类向上转换: 具体 `type` 到其已满足的 object-form `spec`,以及子 object-form `spec` 到其父 object-form `spec`。
- [必须] 对象形状 `spec` 的显式转换资格必须仅依据当前可见契约关系在编译期确定。
- [必须] 对象形状 `spec` 的显式转换一旦成立,编译器必须直接构造静态已知的目标 `spec` 视角; 运行时不得再做候选搜索、试探或回退。
- [禁止] 从父 object-form `spec` 到子 object-form `spec` 的显式转换、无关 `spec` 之间的显式转换、以及依赖运行时具体对象类型才可能成立的对象形状 `spec` 显式转换。
- [必须] union-form 进入站点必须在编译期决定 active member; 当多个 object-form `spec` member 可同时接纳同一源值且不存在精确 member 命中时,必须诊断为歧义,不得按声明顺序兜底。
- [禁止] 当前阶段直接把 union-form 视角值显式转换到共同 object-form `spec`,即使该 union-form 的全部 member 都满足该共同 `spec`。
- [必须] 可调用形状的 `spec` 其参数类型与返回类型必须符合 [Feng 语言 ABI 互操作规范](./feng-interop.md) 中定义的 ABI 函数签名兼容规则，才能标记为 `@abi`。
- [建议] 直接写在 `type` 声明头上的满足关系用于表达“定义者主动承诺”; `fit` 优先用于无法修改原始 `type` 定义时的非侵入适配。

## 6 编译期

- 编译器必须检查 `spec` 声明头右侧是否仅包含 `spec`。
- 编译器必须区分 object-form、callable-form 与 union-form `spec`,并按各自语法形态解析。
- 编译器必须检查 union-form member 列表是否合法,拒绝少于两个 member、`void` member 与 `{}` 块体。
- 编译器必须对 union-form member 拍平嵌套 union-form、去重并保持声明顺序。
- 编译器必须检查 object-form `spec` 的父 `spec` 列表中每一项是否均为 object-form `spec`，并拒绝 callable-form `spec` 与 union-form `spec` 出现在父 `spec` 列表中。
- 编译器必须检查 `type` 声明头与契约适配 `fit` 的右侧是否全部为 object-form `spec`,并拒绝 callable-form `spec` 与 union-form `spec`。
- 编译器必须检查 `type` 对目标 `spec` 的字段与方法是否满足精确匹配规则。
- 编译器必须在 Parser 阶段拒绝 object-form `spec` 体内的 `static` 成员声明。
- 编译器必须检查并拒绝 `spec` 循环声明满足关系。
- 编译器必须检查并拒绝“同名同参数顺序但返回值不一致”的多 `spec` 方法冲突。
- 编译器必须在 object-form `spec` 的父子闭包成员收集中对“同名且签名完全一致”的方法按“子 `spec` 优先”去重,避免把继承覆盖关系误判为多重重载歧义。
- 编译器必须检查并拒绝 `spec` 列表中的重复项。
- 编译器必须在语义分析阶段检查并拒绝 object-form `spec` 中的构造器与终结器声明。
- 编译器必须检查并拒绝在对象形状 `spec` 上使用 `@abi` 或调用方式注解。
- 编译器必须区分“未绑定可调用值 → callable-form `spec`”的结构匹配与“callable-form `spec` → callable-form `spec`”的名义匹配。
- 编译器必须仅在两个 callable-form `spec` 的实例化后签名完全一致时接受显式转换,并在语义分析阶段拒绝其他 callable-form `spec` 转换。
- 编译器必须把实例化后签名完全一致的 callable-form `spec` 显式转换 lower 为零转发的目标视角重解释; 不得为该转换生成新的 wrapper/closure,也不得让转换后的每次调用比转换前多一层 invoke forwarding。
- 编译器必须在语义分析阶段根据当前可见契约关系判定对象形状 `spec` 的显式转换是否属于允许的向上转换,并拒绝父到子、无关 `spec` 或依赖运行时对象具体类型的转换。
- 编译器必须在 union-form 进入站点按精确 member 优先、可证向上转换候选与歧义诊断规则确定 active member。
- 编译器必须在 union-form `if 目标值 { ... }` 中只接受 union member 类型标签与 `else`,拒绝字面量标签和区间标签。
- 编译器必须按 [Feng 语言 ABI 互操作规范](./feng-interop.md) 校验可调用形状的 `@abi spec` 的参数类型与返回类型是否满足 ABI 函数签名兼容规则。
- 编译器必须在语义分析阶段对以上违规报错并阻止通过。

## 7 运行时

- 无初始值的 `spec` 绑定会自动得到该 `spec` 的默认 witness。
- 默认 witness 由编译器自动生成,与对应 `spec` 属于同一模块,但对开发者不可见也不可显式引用; 每个 `spec` 都有对应默认 witness,且必须满足该 `spec` 声明的全部成员与签名要求。
- 字段成员按各自类型的默认零值初始化。
- 返回 `void` 的行为签名提供空实现; 返回非 `void` 的行为签名返回其返回类型默认零值; 若返回值是 `spec` 类型,则返回该 `spec` 对应的默认 witness。
- 每次对 `spec` 类型执行默认初始化时,都会创建该 `spec` 默认 witness 的新实例,不复用共享单例。
- `spec` 值上的 `==` / `!=` 默认比较引用身份,不执行深度比较。
- 代码以 `spec` 视角访问成员或发起调用时,运行时可采用分发表、内联缓存、静态去虚化或其他等价策略完成成员映射与分发。
- callable-form `spec` 的显式转换不引入运行时签名比较、候选搜索、wrapper/closure 分配或额外调用转发; 对实例化后签名完全一致的 callable-form `spec`,转换前后经该值发起的调用开销必须保持同级。
- 对象形状 `spec` 的显式转换不引入运行时满足关系搜索、候选比较或回退; 运行时只执行编译期已选定的 `spec` 视角构造与成员分发。
- union-form 统一映射为既有 `aggregate-with-managed-slots` 顶层值模型,首版值布局为 `tag + _fwd + payload`; `tag` 表达 active member identity,`_fwd` 只表达当前 payload 生命周期路径,不得把 union-form 实现为新的 runtime top-level value kind。
- 运行时实现不强制绑定单一机制; 只要求满足高性能、0 开销或极低开销、ABI 稳定。

## 8 关联

- [Feng 语言 `fit` 规范](./feng-fit.md): `fit` 适配、导出与作用域规则。
- [Feng 语言类型规范](./feng-type.md): 类型系统与成员类型引用规则。
- [Feng 联合类型规范](./feng-union-type.md): union-form 的成员选择、收窄、泛型约束与布局细则。
- [Feng 语言核心规范](./feng-language.md): 语言总规范与跨章节总约束。
