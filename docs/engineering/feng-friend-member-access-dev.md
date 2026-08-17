# Feng `@friend` 成员访问开发草案

> **状态**：待 Review，尚未实施。
>
> **文档定位**：本文只用于确认 `@friend` 的需求边界、编译器方案和实施
> TODO。Review 通过后，应先把正式语义写入权威规范，再修改代码和测试。
>
> **核心范围**：`@friend` 只能标注 `seal` 成员，使注解中列出的具体
> friend type 可以越过该成员自身的 `seal` 检查；它不改变 module、owner、
> `fit`、`spec`、泛型、重载、可变性或其他既有规则。

## 1. 背景

Feng 当前的具体 type `seal` 成员仅允许所属 type 自身访问；同 module 的其他
type 也不能访问。`spec seal` 则由实现该 spec 的 type 通过 spec 视角访问。
这些规则能够表达严格私有和契约实现域访问，但不能表达“只向少数明确类型放行
某一个成员”。

本草案增加内建注解 `@friend`：

```feng
type Vault {
  @friend(Reader, Auditor)
  seal var token: string;

  @friend(Reader)
  seal static func load(): Vault {
    // ...
  }
}
```

`Reader` 和 `Auditor` 获得的只是对应成员的定向访问权。`Vault` 的其他 `seal`
成员仍然保持原有私有语义。

## 2. 设计目标

1. 增加内建注解 `@friend(TypeA, TypeB, ...)`，一次声明一个或多个 friend type。
2. `@friend` 只能标注显式声明为 `seal` 的成员。
3. 支持具体 type 的实例字段、静态字段、实例方法和静态方法。
4. 支持 object-form spec 的实例字段、静态字段、实例方法和静态方法，包括
   `spec seal` 成员。
5. 支持 fit 块声明的实例方法和静态方法；fit 仍不新增字段能力。
6. friend type 自身的实例方法和静态方法可以访问被授权成员。
7. 与 `@friend` 成员位于同一包的 `fit FriendType` 实例方法和静态方法，也可以
   使用该 friend type 的授权。
8. `@friend` 参数使用类型位语法和类型身份，不使用运行时表达式语义。
9. 在声明阶段检查被授权成员的完整签名是否能被每个 friend type 使用。
10. 授权只发生在编译期，不增加运行时分派、对象字段、方法表或访问检查。
11. `@friend` 不导出到 package-public `.ft`，不形成跨包公开能力。

## 3. 非目标

本草案不处理：

- type 级整体友元；
- module 级或 package 级友元；
- 继承、`protected` 或隐式友元传播；
- 因实现同一个 spec、存在无关 fit 或存在 `@mixable` 关系而自动获得友元；
- friend type 的 friend 自动获得访问权；
- 通过 `@friend` 穿透 owner type、owner spec、module 或 fit 的既有可见性；
- 修改具体 type 普通 `seal` 成员、`spec seal` 或 `@mixable seal` 的既有规则；
- 修改 spec requirement 满足、witness 构造或具体实现成员的可见性；
- 将 `@friend` 记录到 package-public `.ft`；
- 运行时反射或动态修改 friend 集合。

## 4. 声明语法与类型参数

### 4.1 基本语法

```feng
@friend(B, C, D)
seal func execute(value: Input): Output {
  // ...
}
```

`@friend` 至少需要一个参数。每个参数都在类型位语境中解析，最终必须表示一个
具体 type；支持短名、导入别名、完整 module 路径和泛型实参等现有具名类型形式：

```feng
@friend(Helper)
@friend(other.Helper)
@friend(Box<T>)
@friend(Box<int>)
```

短名和完整名只影响现有名称解析。只要最终解析为同一个类型，授权语义完全相同。
`@friend` 不按源码文本比较，也不以 friend type 所在文件当前是否已经写出某个
`import` 作为授权条件。

### 4.2 类型身份

friend 参数按 `FengTypeRef` 解析并进行普通类型合法性检查：

- 根类型必须解析为具体 type，不能以 spec、内建标量、数组、指针或单独的类型
  参数作为 friend 主体；
- `Box` 与 `Box<T>` 按现有名称和泛型 arity 规则解析，不能互相替代；
- 泛型实参参与类型身份比较；
- owner type、spec 或 fit 的有效类型参数可以出现在 friend 类型位中；
- 未绑定类型参数必须按普通类型解析报错，不能解释为“任意实例”；
- 访问授权使用完成 owner 泛型代入后的语义类型相等判断。

例如，泛型 owner 上的 `@friend(Box<T>)` 在 `Owner<int>` 视角下表示
`Box<int>`，不能自动授权 `Box<string>`。

### 4.3 AST 表达

当前通用注解参数按 `FengExpr` 保存，但 `@friend` 参数是类型位。实现时不得先把
类型伪装成普通表达式再进行不完整的反向识别。Parser/AST 应提供带类别的统一注解
参数表示：既有注解继续保存表达式参数，`@friend` 保存 `FengTypeRef` 参数，并由
统一的释放、dump 和语义遍历逻辑处理。

## 5. 合法标注位置

### 5.1 type 成员

以下位置合法，但成员必须显式为 `seal`：

```feng
type Vault {
  @friend(Reader)
  seal let id: int;

  @friend(Reader)
  seal static var current: Vault;

  @friend(Reader)
  seal func read(): int {
    return self.id;
  }

  @friend(Reader)
  seal static func create(): Vault {
    return Vault {};
  }
}
```

成员缺省可见性在 Feng 中仍是 `open`。因此省略 `seal` 或显式写 `open` 后使用
`@friend`，均应在编译期拒绝；`@friend` 不改变默认可见性。

### 5.2 object-form spec 成员

object-form spec 的字段、方法及其静态形式可以使用 `@friend`：

```feng
open spec Drawable {
  @friend(Renderer)
  seal func draw(): void;

  @friend(Renderer)
  seal static func reset(): void;
}
```

friend type 通过 `Drawable` 静态视角访问这些成员，调用仍使用现有 witness。
该授权不要求 friend type 自己实现 `Drawable`，也不授予它通过具体实现 type
视角访问实现成员的权限。

`@friend` 不改变 requirement 满足规则：spec requirement 与 type 实现成员的
匹配、实现成员可见性兼容和 witness 选择继续执行现有规则。

### 5.3 fit 成员

fit 块中的实例方法和静态方法可以标注 `@friend`：

```feng
fit ExternalView {
  @friend(ViewHelper)
  seal func inspect(): string {
    // ...
  }
}
```

此时 member owner 是 fit 声明，owner module 是 fit 块所在 module，而不是被扩展
type 所在 module。被扩展 type 可以来自其他包，不改变 friend 的判断规则。

`@friend` 不扩大 fit 自身的作用域。访问点仍须先满足 fit 的现有可见性、导入和
激活规则。

### 5.4 构造函数和终结器

构造函数和终结器均禁止使用 `@friend`：

- 终结器不存在需要由 friend type 直接调用的合法场景；
- friend 构造需求可以由 seal 构造函数配合 `@friend` seal static 工厂方法表达，
  首版不再为构造函数增加直接授权入口。

```feng
type Session {
  seal func Session() {
    // ...
  }

  @friend(SessionFactory)
  seal static func create(): Session {
    return Session();
  }
}
```

## 6. 授权语义

### 6.1 精确成员授权

对 owner `O` 的 seal 成员 `M` 和 `@friend` 中列出的类型 `F`，成员访问仅在以下
条件全部满足时放行：

```text
1. module、owner type/spec 和 fit 等上层声明按既有规则可访问；
2. M 本身是显式 seal 成员；
3. 当前访问点位于 F 自身声明的实例方法或静态方法中，或者位于与 M 同包声明、
   目标类型为 F 的 fit 实例方法或静态方法中；
4. 当前词法 type 或当前 fit 目标经泛型代入后与 friend 类型 F 语义相等；
5. fit 访问时，被授权成员的完整签名对 fit 声明 module 可用；
6. 普通成员查找、静态性、可变性、泛型和重载规则全部通过。
```

访问判断可概括为：

```text
ordinary_member_accessible
||
(member.is_seal && current_friend_subject_type matches member.friend_types)
```

第二个分支只替代成员自身的 seal 检查，不能替代第一个条件中的任何上层检查。

### 6.2 当前 friend 授权主体

friend 身份来自访问代码实际声明所在的具体 type，或当前同包 fit 的目标 type：

- type 的实例方法可以使用该 type 的 friend 权限；
- type 的静态方法可以使用该 type 的 friend 权限；
- 同包 `fit FriendType` 的实例方法和静态方法可以使用 `FriendType` 的 friend 权限；
- 顶层函数不属于任何 friend type；
- 依赖包中的 fit 不能恢复或使用未导出的 friend 权限。

`fit FriendType` 获得的只是 `FriendType` 在具体 `@friend` 成员上的定向授权身份，
不会使 fit 成为目标 type 自身，也不会使 fit 访问目标 type 或其他 owner 的普通
seal 成员。fit 方法自身被标注 `@friend` 是授权其他 type 访问该 fit 方法，和本节
定义的“fit 使用目标 type 的 friend 身份”是两个方向不同的能力。

```feng
type Vault {
  @friend(Helper)
  seal var token: string;

  seal var internalState: int;
}

type Helper {}

fit Helper {
  func read(vault: Vault): string {
    return vault.token;         // 允许：使用 Helper 的定向 friend 权限
  }

  static func readStatic(vault: Vault): string {
    return vault.token;         // 允许：fit static 方法同样使用目标身份
  }

  func invalid(vault: Vault): int {
    return vault.internalState; // 拒绝：未标注对应 @friend
  }
}
```

### 6.3 不传播

`@friend` 不建立 type 间的整体关系，也不传播：

```text
A 是 M 的 friend
B 是 A 的 friend
```

不能推出 `B` 可以访问 `M`。同样，实现相同 spec、mix 相同来源或被同一个 fit
扩展，都不能产生额外 friend 权限。

### 6.4 重载与成员选择

同名成员形成重载集合时，必须先按普通可见性与 `@friend` 授权过滤候选，再执行
参数匹配和最佳重载选择：

- 对 friend type，可保留对应的 seal 候选；
- 对其他访问点，该候选继续视为不可访问；
- 不可访问的 friend/seal 候选不得遮蔽同名 open 候选；
- 只有所有结构候选都不可访问时，才报告成员不可访问。

字段、方法、静态成员和 spec 视角成员必须复用同一个 friend 授权谓词，不能在
某一条调用路径单独跳过可见性检查。

## 7. friend 成员签名可见性

### 7.1 检查目标

`@friend` 将一个 seal 成员定向暴露给其他 type，因此成员完整签名必须能被每个
friend type 使用。这里检查的是 friend 是否具备引用该类型的能力，不要求 friend
所在源码文件当前已经写出对应短名 `import`。

定义：

- `O`：被标注成员的 owner type、owner spec 或 owner fit；
- `OM`：`O` 的声明 module；
- `F`：当前检查的一个 friend type；
- `FM`：`F` 的声明 module；
- `FitM`：使用 `F` 授权的某个 `fit F` 的声明 module；
- `R`：成员完整签名中递归出现的一个已解析具名类型；
- `RM`：`R` 的声明 module。

### 7.2 friend type 自身的两条规则

对每个 friend type 独立执行以下规则：

1. `FM == OM`：该 friend type 直接通过签名可见性检查。
2. `FM != OM`：递归检查成员完整签名；如果存在
   `RM == OM && R.visibility != open`，则拒绝该 `@friend` 声明。

除规则 2 明确拒绝的情况外，其他引用类型通过。

第二条成立的原因是：

- owner 能成功引用其他 module 的类型，已经说明该类型具备相应跨 module 引用条件；
- 同包 friend 可以自行导入该 module 或使用完整路径；
- `seal module` 中的 `open type` 是包内可见，其他同包 module 仍可导入和使用；
- 真正需要额外阻止的是 owner module 内的非 open 类型泄漏给其他 module 的 friend。

规则只比较解析后的声明和 module 身份，与源码使用短名、别名还是完整路径无关。

### 7.3 `fit FriendType` 的签名规则

第 7.2 节的 `@friend(F)` 声明检查通过后，`fit F` 仍可能声明在与 `F` 不同的
module，不能直接沿用 `FM` 判断 fit 方法体是否能使用成员签名。因此，fit 使用
friend 权限时，还要在访问点按相同规则检查 fit 声明 module：

1. `FitM == OM`：该 fit 访问通过签名可见性检查。
2. `FitM != OM`：如果成员完整签名中存在
   `RM == OM && R.visibility != open`，则该 fit 不能使用此 friend 权限。

该检查是第 7.2 节声明检查之后的附加访问条件，不会反过来挽救原本非法的
`@friend(F)` 声明，也不会使合法声明整体失效；它只拒绝签名在当前 fit module
不可用的具体访问。这样可以避免其他 module 的 `fit F` 通过类型推导间接取得
owner module 的不可见签名类型。

### 7.4 多 friend

多个 friend 分别检查，只要任意一个失败，整个成员声明失败：

```feng
module app.owner;

type Hidden {}

type Service {
  @friend(LocalHelper, app.other.RemoteHelper)
  seal func load(): Hidden {
    // ...
  }
}

type LocalHelper {}
```

`LocalHelper` 根据规则 1 通过；`RemoteHelper` 根据规则 2 失败，因此 `load` 的
`@friend` 声明整体非法。

### 7.5 递归范围

完整签名检查至少包括：

- 字段的显式类型或推导类型；
- 方法参数；
- 显式或推导返回类型；
- 方法泛型参数约束；
- 泛型实参；
- 数组元素类型和指针目标类型；
- 其他现有类型结构中递归包含的具名类型。

内建类型直接通过。类型参数本身不作为具名声明比较，但其约束继续递归检查。

### 7.6 示例

同 module friend 可以使用 owner module 的 seal 类型：

```feng
module app.owner;

type Hidden {}
type Helper {}

type Service {
  @friend(Helper)
  seal func load(): Hidden {
    // ...
  }
}
```

跨 module friend 不能使用 owner module 的 seal 类型：

```feng
module app.owner;

type Hidden {}

open type Service {
  @friend(app.other.Helper)
  seal func load(): Hidden {
    // 编译期拒绝
  }
}
```

owner module 内的 open 类型允许同包其他 module 的 friend 使用，即使 owner
module 本身是 seal：

```feng
module app.owner;

open type Shared {}

open type Service {
  @friend(app.other.Helper)
  seal func load(): Shared {
    // 通过：Shared 是包内可见类型
  }
}
```

## 8. module、package 与 `.ft`

### 8.1 上层可见性不变

`@friend` 不穿透以下边界：

- owner module 不可访问时，不能访问成员；
- owner type 或 spec 不可访问时，不能访问其成员；
- fit 未按现有规则可见或激活时，不能访问 fit 成员；
- 具体 receiver、静态目标或 spec 视角不成立时，不能只凭 friend 绕过成员查找。

### 8.2 跨包参数与有效授权

`@friend` 参数执行普通类型位解析，因此语法上可以引用依赖包中的类型。但 Feng
禁止包循环依赖：owner 包能引用外部 friend type，意味着 owner 包依赖该外部包；
外部 type 自身所在包不能再反向依赖 owner 包来访问这个成员。

owner 包仍可在本包声明 `fit external.pkg.FriendType`。该 fit 的实例方法和静态
方法可以在本包编译期间使用 `FriendType` 的 friend 身份，因为 `@friend` 声明和
fit 方法体同时可见。此能力不会离开 owner 包：

- `@friend` 事实不进入 package-public `.ft`；
- 外部 friend type 自身的包不能恢复该授权；
- 其他依赖包声明的 `fit FriendType` 也不能恢复该授权；
- owner 包内的 fit 仍须通过第 7.3 节按其声明 module 执行签名检查。

因此，编译器不需要限制 friend type 必须与 owner 同包；需要限制的是
`fit FriendType` 对 friend 权限的使用必须发生在声明该 `@friend` 成员的同一个包。

### 8.3 不导出 friend 事实

package-public `.ft` 不记录 `@friend` 参数或 friend 授权：

- `@friend` 不改变成员本身的现有导出选择规则：普通具体 seal 成员、完整 spec
  契约中的 seal requirement、`@mixable seal static` 等各自继续执行既有规则；
- `@friend` 不使 seal 成员成为 package-public API；
- 不增加 friend section、friend type 表或新的运行时 ABI；
- dependency consumer 不能从 `.ft` 恢复或使用 provider 内部 friend 关系，包括
  不能通过 consumer 自己声明的 `fit FriendType` 取得该授权。

## 9. 编译器设计

### 9.1 Lexer 与 Parser

- 将 `friend` 加入内建注解表，形成稳定的 `FENG_ANNOTATION_FRIEND`；
- 注解解析根据参数类别调用类型位解析，保留完整 `FengTypeRef`；
- type 和 fit 现有成员注解位置继续复用；
- object-form spec 成员解析增加与 type/fit 一致的成员注解入口；
- AST dump 和释放逻辑覆盖类型参数；
- 既有表达式参数注解的 AST 与行为保持不变。

### 9.2 语义归一化

语义阶段应为每个 `@friend` 成员建立规范化 friend type 集合：

- 在 owner type/spec/fit 的类型参数作用域和声明文件名称解析环境中解析；
- 保存或可稳定恢复解析后的声明身份与完整类型实参；
- 授权比较复用现有语义类型相等和泛型代入能力；
- member owner 查询统一返回 owner decl、owner module 和 owner program；
- 不以字符串、短名、完整名或 `.ft` 收录情况作为类型身份。

### 9.3 声明检查

统一的 `@friend` 声明检查至少验证：

- 参数非空且每个参数是合法类型位；
- 标注目标是允许的 type/spec/fit 成员；
- 成员显式为 `seal`；
- friend 类型全部成功解析；
- 对每个 friend 执行第 7 节两条签名可见性规则。

现有公开签名递归遍历已经覆盖具名类型、泛型实参、数组、指针、参数、返回值和
约束。实现应复用或抽取其遍历框架，但不能直接套用“有效可见性等级不小于公开面”
的比较条件；friend type 自身使用第 7.2 节按 `FM/OM/RM` 判断的两条规则，
`fit FriendType` 访问点使用第 7.3 节按 `FitM/OM/RM` 判断的同构规则。

### 9.4 统一访问查询

语义层应提供一个可复用的 friend seal 授权查询，输入至少包括：

- 当前解析上下文；
- 当前词法 type 或当前 fit 目标及其实参；
- 当前 fit 与成员 owner 是否属于同一包；
- owner type/spec/fit；
- receiver 或静态 owner 的具体化类型实参；
- 待访问成员及其规范化 friend type 集合。

该查询应统一服务于：

- 具体 type 实例字段和实例方法；
- 具体 type 静态字段和静态方法；
- object-form spec 实例/静态字段和方法；
- fit 实例/静态方法；
- 重载候选过滤；
- 编译器和 LSP 共用的可访问成员视图。

不能只在最终调用发码前跳过 seal 检查，否则字段访问、方法值、重载或工具结果会
与调用语义不一致。

### 9.5 Codegen

Codegen 不应增加 friend 分支。通过语义检查的访问继续使用原有发码路径：

- 具体字段仍按现有字段访问发码；
- 具体方法和静态方法仍按现有直接调用规则发码；
- spec 成员仍通过现有 witness 发码；
- fit 方法仍使用现有 fit 调用目标；
- 非法访问必须在语义阶段拒绝。

因此本能力不增加运行时开销，也不修改 runtime ABI。

## 10. 诊断要求

实施时应为以下错误提供稳定、可区分的编译期诊断，并同步错误码规范：

- `@friend` 没有参数；
- 参数不是合法类型位或类型无法解析；
- `@friend` 用于不支持的声明位置；
- `@friend` 用于 open 或缺省 open 成员；
- friend 成员签名对某个 friend type 不可用；
- 非 friend 访问 seal 成员时继续使用现有成员不可访问诊断。

诊断应指向注解、非法参数或导致签名泄漏的具体类型引用，并在消息中包含成员名、
friend type 和不可用类型，避免只报告泛化的“annotation invalid”。具体错误码在
Review 后随权威错误码文档一起确定，本文不提前占用编号。

## 11. 测试计划

### 11.1 Lexer / Parser / AST

- `@friend` 识别为内建注解；
- 单个和多个 friend type；
- 短名、别名、完整路径和泛型类型位；
- type 实例/静态字段和方法；
- object-form spec 实例/静态字段和方法；
- fit 实例/静态方法；
- AST dump 保留类型参数结构；
- 无参数、表达式参数和非法类型位诊断；
- open、缺省 open 和非法声明位置诊断；
- 构造函数和终结器使用 `@friend` 均被拒绝；
- 既有注解参数解析不退化。

### 11.2 Semantic

- friend type 的实例方法和静态方法均能访问授权字段、方法和静态成员；
- 非 friend type、顶层函数和普通外部调用仍被拒绝；
- 同包 `fit FriendType` 的实例方法和静态方法均能访问授权成员；
- `fit FriendType` 仍不能访问未标注对应 `@friend` 的普通 seal 成员；
- 其他包的 `fit FriendType` 不能恢复或使用 friend 权限；
- 每个成员独立授权，不能访问 owner 的其他 seal 成员；
- 多个 friend 均可访问同一成员；
- friend 权限不传递；
- owner module、owner type/spec 和 fit 可见性不能被穿透；
- 重载先过滤不可访问候选，open 候选不被 seal friend 候选遮蔽；
- 字段可写性、静态性和泛型检查保持不变；
- 泛型 owner 的 friend 类型在代入后精确匹配，不扩大到其他实例。

### 11.3 签名可见性

- friend 与 owner 同 module 时，签名使用本 module seal 类型通过；
- friend 与 owner 不同 module 时，签名使用 owner module seal 类型拒绝；
- friend 与 owner 不同 module 时，签名使用 owner module open 类型通过；
- owner module 为 seal、引用类型为 open 时，同包跨 module friend 通过；
- 签名引用其他 module 已成功解析的类型时通过；
- 多 friend 中同 module friend 通过、跨 module friend 失败时，整个声明失败；
- `fit FriendType` 与 owner 同 module 时，签名使用本 module seal 类型通过；
- `fit FriendType` 与 owner 不同 module 时，签名使用 owner module seal 类型的
  具体访问被拒绝；
- 递归覆盖泛型实参、约束、数组、指针、字段、参数及显式/推导返回类型；
- 检查结果不受短名、别名或完整路径写法影响。

### 11.4 Spec / witness

- friend type 通过 spec 视角访问 `@friend seal` 实例和静态成员；
- friend type 不需要实现该 spec；
- 非 friend 且非既有 spec 实现域访问者仍被拒绝；
- friend 不得通过具体实现 type 视角访问其 seal 实现成员；
- requirement 满足与 witness 选择结果不因 `@friend` 改变；
- spec 字段读写继续遵守既有 mutability 和 witness 规则。

### 11.5 Fit / 跨包 / Symbol

- type 位于外部包时，本包 fit 的 `@friend seal` 方法仍按 owner fit module 判断；
- fit 不可见或未激活时，friend 不能穿透访问；
- 本包 `fit` 可以使用外部 friend type 的身份访问本包 `@friend` 成员；
- package-public `.ft` 不记录 friend 参数和授权；
- dependency consumer 不能通过 `.ft` 获得 provider friend 能力；
- dependency consumer 自己声明的 `fit FriendType` 也不能恢复 provider friend 能力；
- symbol 导出和恢复的其他 annotation 行为不变。

### 11.6 Codegen / FCTS / 回归

- type 字段、方法和静态成员的成功 friend 访问产生正确运行结果；
- spec friend 调用继续通过 witness 并产生正确结果；
- fit friend 方法调用产生正确结果；
- 泛型 friend 场景正确发码；
- 不增加运行时 helper、动态检查或额外参数；
- 完成任何非文档变更后执行 `make test` 全量回归。

## 12. TODO

### TODO 1：完成 Review 决策并更新正式规范

- [ ] Review 本文的授权边界、类型位参数和签名可见性两条规则。
- [ ] 决定重复 friend 参数、多个 `@friend` 注解的归一化或诊断规则。
- [ ] 更新 `docs/specifications/feng-visibility.md`，作为 friend 成员访问和签名
  可见性的主规范。
- [ ] 更新 `docs/specifications/feng-language.md` 的内建注解列表与数量。
- [ ] 在 `docs/specifications/feng-type.md`、`feng-spec.md` 和 `feng-fit.md` 中只补充
  对主规范的引用和各自适用位置，不重复定义授权算法。
- [ ] 更新 parser/annotation 相关规范和错误码文档。

### TODO 2：实现类型位注解参数

- [ ] 为注解参数建立可区分表达式参数与类型参数的通用 AST 表示。
- [ ] 增加 `FENG_ANNOTATION_FRIEND` 内建注解事实。
- [ ] 让 `@friend` 参数使用现有类型位 parser，支持短名、完整路径和泛型类型。
- [ ] 补齐 AST 释放、dump 和相关遍历。
- [ ] 为 object-form spec 成员补齐注解解析入口。
- [ ] 验证其他现有注解的解析和 AST 不变。

### TODO 3：实现声明解析与签名检查

- [ ] 在 owner 泛型作用域中解析、规范化 friend type 集合。
- [ ] 建立可复用的 member owner decl/module/program 查询。
- [ ] 检查合法成员类别、显式 seal 和参数完整性。
- [ ] 明确拒绝构造函数和终结器上的 `@friend`。
- [ ] 复用现有签名类型递归遍历，实现第 7.2 和 7.3 节 friend 规则。
- [ ] 覆盖字段推导类型和 callable 推导返回类型。
- [ ] 增加稳定诊断并更新错误码规范。

### TODO 4：实现统一访问授权

- [ ] 实现基于语义类型身份和泛型代入的 friend 授权谓词。
- [ ] 接入 type 实例/静态字段与方法访问。
- [ ] 接入 object-form spec 实例/静态字段与方法访问。
- [ ] 接入 fit 实例/静态方法访问。
- [ ] 在重载选择前过滤不可访问候选。
- [ ] 验证只有同包 fit 可以使用其目标 type 的 friend 身份。
- [ ] 验证 fit 的 friend 身份只放行对应注解成员，不放行其他普通 seal 成员。
- [ ] 验证上层 module/owner/fit 可见性仍先于 friend 成员授权。

### TODO 5：验证 codegen、Symbol 与工具链

- [ ] 验证所有成功访问继续复用既有 codegen，无新增运行时分支或 ABI。
- [ ] 验证 spec friend 成员继续通过既有 witness 发码。
- [ ] 验证 package-public `.ft` 不记录 friend 参数或授权。
- [ ] 验证 completion、hover、definition 与编译器使用同一可访问成员结果。

### TODO 6：补齐测试并全量回归

- [ ] 按第 11 节补齐 lexer、parser、semantic、symbol、codegen 和 FCTS 用例。
- [ ] 先执行目标测试，确认正向、负向、泛型、spec、fit 和跨 module 边界。
- [ ] 执行 `make test` 全量回归。

## 13. 验收标准

本草案完成实施必须同时满足：

1. `@friend` 只为明确列出的 type 放行明确标注的 seal 成员。
2. type、object-form spec 和 fit 的实例/静态适用位置行为一致。
3. friend 参数严格使用类型位和语义类型身份，不依赖文本或 import 写法。
4. 完整签名对 friend type 和同包 `fit FriendType` 分别执行第 7.2、7.3 节规则，
   递归类型无遗漏。
5. module、owner、fit、spec 满足、witness 和普通 seal 规则均未被扩大。
6. package-public `.ft` 不携带 friend 能力，跨包不能恢复授权。
7. Codegen 与 runtime ABI 无变化，运行时无新增开销。
8. 专项测试和 `make test` 全量回归全部通过。
